/**
 * Small iCalendar (RFC 5545) reader for Pocket's Today screen — enough for Google / iCloud /
 * Outlook feeds: VEVENT with DTSTART/DTEND/DURATION (UTC, TZID or VALUE=DATE), SUMMARY, LOCATION,
 * RRULE (DAILY/WEEKLY/MONTHLY/YEARLY + INTERVAL, COUNT, UNTIL, BYDAY), EXDATE, RECURRENCE-ID
 * overrides and STATUS:CANCELLED. Not a full RFC implementation.
 * Production mirror: neon-fn/scram-api.mjs (icsParse / icsExpand) — keep both in step.
 */

export type IcsTime = {
  /** Wall-clock fields as a UTC epoch (i.e. the local time "pretending" to be UTC). */
  wall: number;
  /** IANA zone for wall time; "UTC" for Z / floating fallback. */
  tz: string;
  allDay: boolean;
};

export type IcsRule = {
  freq: "DAILY" | "WEEKLY" | "MONTHLY" | "YEARLY";
  interval: number;
  count: number | null;
  /** UTC instant (or, for all-day starts, a UTC-midnight wall date) after which nothing occurs. */
  until: number | null;
  byday: { ord: number; day: number }[];
};

export type IcsEvent = {
  uid: string;
  summary: string;
  location: string;
  start: IcsTime;
  /** Duration in ms (wall-clock delta for all-day events); null when the feed gives no end. */
  durationMs: number | null;
  rrule: IcsRule | null;
  /** Excluded occurrence starts, as UTC instants. */
  exdates: number[];
  /** Set on an override of one occurrence of a recurring event (UTC instant it replaces). */
  recurrenceId: number | null;
  cancelled: boolean;
};

export type CalendarEvent = {
  title: string;
  start: string;
  end: string | null;
  all_day: boolean;
  location: string;
  calendar: string;
};

const DAY_MS = 86400000;
const WEEKDAYS = ["SU", "MO", "TU", "WE", "TH", "FR", "SA"];
const MAX_ITERATIONS = 20000;

// ---- Time zones -----------------------------------------------------------------------------

const zoneFormatters = new Map<string, Intl.DateTimeFormat | null>();

function zoneFormatter(tz: string): Intl.DateTimeFormat | null {
  if (zoneFormatters.has(tz)) return zoneFormatters.get(tz)!;
  let f: Intl.DateTimeFormat | null = null;
  try {
    f = new Intl.DateTimeFormat("en-US", {
      timeZone: tz,
      hourCycle: "h23",
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
  } catch {
    f = null;
  }
  zoneFormatters.set(tz, f);
  return f;
}

/** Resolve a TZID to a zone Intl knows ("/mozilla.org/.../Europe/Berlin" → "Europe/Berlin"). */
export function resolveZone(tzid: string | undefined, fallback = "UTC"): string {
  const raw = String(tzid || "").replace(/^"|"$/g, "").trim();
  if (!raw) return fallback;
  if (zoneFormatter(raw)) return raw;
  const m = raw.match(/([A-Za-z_]+\/[A-Za-z_+-]+(?:\/[A-Za-z_+-]+)?)$/);
  if (m && zoneFormatter(m[1]!)) return m[1]!;
  return fallback;
}

/** Offset (ms) of `tz` from UTC at instant `utcMs`. */
function zoneOffset(utcMs: number, tz: string): number {
  if (tz === "UTC") return 0;
  const f = zoneFormatter(tz);
  if (!f) return 0;
  const p: Record<string, number> = {};
  for (const part of f.formatToParts(new Date(utcMs))) {
    if (part.type !== "literal") p[part.type] = Number(part.value);
  }
  const asUtc = Date.UTC(p.year!, p.month! - 1, p.day!, p.hour! % 24, p.minute!, p.second!);
  return asUtc - Math.floor(utcMs / 1000) * 1000;
}

/** Wall-clock time in `tz` (encoded as a UTC epoch) → real UTC instant. */
export function wallToUtc(wall: number, tz: string): number {
  if (tz === "UTC") return wall;
  const o1 = zoneOffset(wall - 0, tz);
  let t = wall - o1;
  const o2 = zoneOffset(t, tz);
  if (o2 !== o1) t = wall - o2;
  return t;
}

function toUtc(t: IcsTime): number {
  return t.allDay ? t.wall : wallToUtc(t.wall, t.tz);
}

// ---- Content lines --------------------------------------------------------------------------

type Prop = { name: string; params: Record<string, string>; value: string };

/** Unfold (RFC 5545 §3.1) and split into content lines. */
export function unfoldLines(text: string): string[] {
  return text
    .replace(/\r\n/g, "\n")
    .replace(/\r/g, "\n")
    .replace(/\n[ \t]/g, "")
    .split("\n")
    .filter((l) => l.length > 0);
}

function parseLine(line: string): Prop | null {
  let i = 0;
  let inQuote = false;
  let colon = -1;
  for (; i < line.length; i++) {
    const ch = line[i];
    if (ch === '"') inQuote = !inQuote;
    else if (ch === ":" && !inQuote) {
      colon = i;
      break;
    }
  }
  if (colon < 0) return null;
  const head = line.slice(0, colon);
  const value = line.slice(colon + 1);
  const parts = head.match(/(?:[^;"]|"[^"]*")+/g) || [];
  const name = (parts[0] || "").toUpperCase();
  const params: Record<string, string> = {};
  for (const p of parts.slice(1)) {
    const eq = p.indexOf("=");
    if (eq > 0) params[p.slice(0, eq).toUpperCase()] = p.slice(eq + 1).replace(/^"|"$/g, "");
  }
  return { name, params, value };
}

export function unescapeText(v: string): string {
  return v.replace(/\\([\\;,nN])/g, (_, c: string) => (c === "n" || c === "N" ? "\n" : c));
}

/** One DATE / DATE-TIME value. `defaultTz` is the calendar's X-WR-TIMEZONE (floating times). */
export function parseIcsTime(
  value: string,
  params: Record<string, string>,
  defaultTz = "UTC",
): IcsTime | null {
  const v = value.trim();
  const d = v.match(/^(\d{4})(\d{2})(\d{2})$/);
  if (d || params.VALUE === "DATE") {
    if (!d) return null;
    return { wall: Date.UTC(+d[1]!, +d[2]! - 1, +d[3]!), tz: "UTC", allDay: true };
  }
  const m = v.match(/^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})?(Z)?$/);
  if (!m) return null;
  const wall = Date.UTC(+m[1]!, +m[2]! - 1, +m[3]!, +m[4]!, +m[5]!, +(m[6] || 0));
  if (m[7]) return { wall, tz: "UTC", allDay: false };
  return { wall, tz: resolveZone(params.TZID, defaultTz), allDay: false };
}

/** ISO-8601 duration (P1D, PT1H30M, P1W) → ms. */
export function parseDuration(v: string): number | null {
  const m = v.trim().match(/^([+-])?P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?$/);
  if (!m) return null;
  const ms =
    (+(m[2] || 0) * 7 + +(m[3] || 0)) * DAY_MS +
    +(m[4] || 0) * 3600000 +
    +(m[5] || 0) * 60000 +
    +(m[6] || 0) * 1000;
  return m[1] === "-" ? -ms : ms;
}

function parseRule(v: string, start: IcsTime, defaultTz: string): IcsRule | null {
  const kv: Record<string, string> = {};
  for (const part of v.split(";")) {
    const eq = part.indexOf("=");
    if (eq > 0) kv[part.slice(0, eq).toUpperCase()] = part.slice(eq + 1);
  }
  const freq = kv.FREQ?.toUpperCase();
  if (freq !== "DAILY" && freq !== "WEEKLY" && freq !== "MONTHLY" && freq !== "YEARLY") return null;
  const interval = Math.max(1, Number.parseInt(kv.INTERVAL || "1", 10) || 1);
  const count = kv.COUNT ? Math.max(0, Number.parseInt(kv.COUNT, 10) || 0) : null;
  let until: number | null = null;
  if (kv.UNTIL) {
    const t = parseIcsTime(kv.UNTIL, {}, defaultTz);
    if (t) {
      // Date-only UNTIL is inclusive of that whole day.
      until = t.allDay
        ? start.allDay
          ? t.wall
          : wallToUtc(t.wall + DAY_MS - 1, start.tz)
        : toUtc(t);
    }
  }
  const byday: { ord: number; day: number }[] = [];
  for (const tok of (kv.BYDAY || "").split(",")) {
    const m = tok.trim().toUpperCase().match(/^([+-]?\d{1,2})?(SU|MO|TU|WE|TH|FR|SA)$/);
    if (m) byday.push({ ord: m[1] ? Number.parseInt(m[1], 10) : 0, day: WEEKDAYS.indexOf(m[2]!) });
  }
  return { freq, interval, count, until, byday };
}

/** Parse every VEVENT in an .ics body. Unreadable events are skipped. */
export function parseIcs(text: string): IcsEvent[] {
  const lines = unfoldLines(text);
  let defaultTz = "UTC";
  for (const l of lines) {
    if (/^X-WR-TIMEZONE[:;]/i.test(l)) {
      const p = parseLine(l);
      if (p) defaultTz = resolveZone(p.value, "UTC");
      break;
    }
  }
  const out: IcsEvent[] = [];
  let props: Prop[] | null = null;
  let depth = 0; // nesting inside VEVENT (VALARM etc.)
  for (const line of lines) {
    const p = parseLine(line);
    if (!p) continue;
    if (p.name === "BEGIN") {
      if (p.value.trim().toUpperCase() === "VEVENT" && !props) {
        props = [];
        depth = 0;
      } else if (props) depth++;
      continue;
    }
    if (p.name === "END") {
      if (!props) continue;
      if (depth > 0) {
        depth--;
        continue;
      }
      if (p.value.trim().toUpperCase() === "VEVENT") {
        const ev = buildEvent(props, defaultTz);
        if (ev) out.push(ev);
        props = null;
      }
      continue;
    }
    if (props && depth === 0) props.push(p);
  }
  return out;
}

function buildEvent(props: Prop[], defaultTz: string): IcsEvent | null {
  const first = (n: string) => props.find((p) => p.name === n);
  const dtstart = first("DTSTART");
  if (!dtstart) return null;
  const start = parseIcsTime(dtstart.value, dtstart.params, defaultTz);
  if (!start) return null;
  let durationMs: number | null = null;
  const dtend = first("DTEND");
  const dur = first("DURATION");
  if (dtend) {
    const end = parseIcsTime(dtend.value, dtend.params, defaultTz);
    if (end) durationMs = start.allDay ? end.wall - start.wall : toUtc(end) - toUtc(start);
  } else if (dur) {
    durationMs = parseDuration(dur.value);
  }
  if (durationMs == null && start.allDay) durationMs = DAY_MS;
  if (durationMs != null && durationMs < 0) durationMs = 0;
  const rr = first("RRULE");
  const exdates: number[] = [];
  for (const p of props.filter((x) => x.name === "EXDATE")) {
    for (const v of p.value.split(",")) {
      const t = parseIcsTime(v, p.params, defaultTz);
      if (t) exdates.push(t.allDay && !start.allDay ? wallToUtc(t.wall + (start.wall % DAY_MS), start.tz) : toUtc(t));
    }
  }
  const rid = first("RECURRENCE-ID");
  const ridTime = rid ? parseIcsTime(rid.value, rid.params, defaultTz) : null;
  return {
    uid: first("UID")?.value.trim() || "",
    summary: unescapeText(first("SUMMARY")?.value || "").trim(),
    location: unescapeText(first("LOCATION")?.value || "").trim(),
    start,
    durationMs,
    rrule: rr ? parseRule(rr.value, start, defaultTz) : null,
    exdates,
    recurrenceId: ridTime ? toUtc(ridTime) : null,
    cancelled: (first("STATUS")?.value || "").trim().toUpperCase() === "CANCELLED",
  };
}

// ---- Recurrence -----------------------------------------------------------------------------

function daysInMonth(y: number, m0: number): number {
  return new Date(Date.UTC(y, m0 + 1, 0)).getUTCDate();
}

/** nth (1-based, negative from the end) `weekday` of month → day of month, or null. */
function nthWeekday(y: number, m0: number, weekday: number, ord: number): number | null {
  const dim = daysInMonth(y, m0);
  if (ord > 0) {
    const firstDow = new Date(Date.UTC(y, m0, 1)).getUTCDay();
    const day = 1 + ((weekday - firstDow + 7) % 7) + (ord - 1) * 7;
    return day <= dim ? day : null;
  }
  const lastDow = new Date(Date.UTC(y, m0, dim)).getUTCDay();
  const day = dim - ((lastDow - weekday + 7) % 7) + (ord + 1) * 7;
  return day >= 1 ? day : null;
}

/**
 * Occurrence starts (wall-clock epochs, ascending) for a rule, starting at the event's DTSTART.
 * `stopWall` lets open-ended rules stop once past the window; COUNT/UNTIL stop them earlier.
 */
function* occurrences(ev: IcsEvent, stopWall: number): Generator<number> {
  const rule = ev.rrule!;
  const start = ev.start.wall;
  const tod = ((start % DAY_MS) + DAY_MS) % DAY_MS;
  const s = new Date(start);
  const sy = s.getUTCFullYear();
  const sm = s.getUTCMonth();
  const sd = s.getUTCDate();
  let emitted = 0;
  let iterations = 0;
  const done = (wall: number) => {
    if (rule.count != null && emitted >= rule.count) return true;
    if (wall > stopWall) return true;
    if (rule.until != null) {
      const utc = ev.start.allDay ? wall : wallToUtc(wall, ev.start.tz);
      if (utc > rule.until) return true;
    }
    return false;
  };

  // Open-ended DAILY/WEEKLY rules that began long ago: skip whole periods up to the window.
  let skipPeriods = 0;
  const periodMs = (rule.freq === "DAILY" ? 1 : 7) * DAY_MS * rule.interval;
  if (rule.count == null && (rule.freq === "DAILY" || rule.freq === "WEEKLY")) {
    const lag = stopWall - 40 * DAY_MS - start;
    if (lag > 0) skipPeriods = Math.floor(lag / periodMs);
  }

  for (let k = 0; ; k++) {
    if (++iterations > MAX_ITERATIONS) return;
    const step = k + skipPeriods;
    let candidates: number[] = [];
    if (rule.freq === "DAILY") {
      candidates = [start + step * periodMs];
    } else if (rule.freq === "WEEKLY") {
      const days = rule.byday.length ? rule.byday.map((b) => b.day) : [s.getUTCDay()];
      // Weeks start on Monday (WKST=MO default).
      const startDow = (s.getUTCDay() + 6) % 7;
      const weekStart = start - tod - startDow * DAY_MS + step * periodMs;
      candidates = [...new Set(days)]
        .map((d) => weekStart + ((d + 6) % 7) * DAY_MS + tod)
        .sort((a, b) => a - b);
    } else if (rule.freq === "MONTHLY") {
      const mi = sm + step * rule.interval;
      const y = sy + Math.floor(mi / 12);
      const m0 = ((mi % 12) + 12) % 12;
      if (rule.byday.length) {
        const days: number[] = [];
        for (const b of rule.byday) {
          if (b.ord) {
            const d = nthWeekday(y, m0, b.day, b.ord);
            if (d) days.push(d);
          } else {
            for (let d = 1; d <= daysInMonth(y, m0); d++) {
              if (new Date(Date.UTC(y, m0, d)).getUTCDay() === b.day) days.push(d);
            }
          }
        }
        candidates = [...new Set(days)].sort((a, b) => a - b).map((d) => Date.UTC(y, m0, d) + tod);
      } else if (sd <= daysInMonth(y, m0)) {
        candidates = [Date.UTC(y, m0, sd) + tod];
      }
    } else {
      const y = sy + step * rule.interval;
      if (sd <= daysInMonth(y, sm)) candidates = [Date.UTC(y, sm, sd) + tod];
    }
    if (!candidates.length) {
      if (rule.freq === "MONTHLY" || rule.freq === "YEARLY") {
        // Skipped (e.g. the 31st in a short month) — keep going unless clearly past the window.
        const probe =
          rule.freq === "YEARLY"
            ? Date.UTC(sy + step * rule.interval, 0, 1)
            : Date.UTC(sy, sm + step * rule.interval, 1);
        if (probe > stopWall) return;
      }
      continue;
    }
    for (const c of candidates) {
      if (c < start) continue;
      if (done(c)) return;
      emitted++;
      yield c;
    }
  }
}

/**
 * Events overlapping [from, to) for one calendar, as Pocket's Today shape. All-day events are
 * dated at 00:00 UTC (all_day: true, end exclusive); they're matched against a window widened
 * by 14 h so "today" survives every time zone — the device filters by its own local date.
 */
export function expandEvents(
  events: IcsEvent[],
  from: Date,
  to: Date,
  calendar: string,
): CalendarEvent[] {
  const fromMs = from.getTime();
  const toMs = to.getTime();
  const SLACK = 14 * 3600000;
  const overrides = new Set<string>();
  for (const ev of events) if (ev.recurrenceId != null) overrides.add(`${ev.uid}|${ev.recurrenceId}`);
  const out: CalendarEvent[] = [];

  const emit = (ev: IcsEvent, startUtc: number) => {
    const dur = ev.durationMs;
    const endUtc = dur == null ? null : startUtc + dur;
    const lo = ev.start.allDay ? fromMs - SLACK : fromMs;
    const hi = ev.start.allDay ? toMs + SLACK : toMs;
    const overlaps = endUtc == null || endUtc === startUtc ? startUtc >= lo && startUtc < hi : startUtc < hi && endUtc > lo;
    if (!overlaps) return;
    out.push({
      title: ev.summary || "Busy",
      start: new Date(startUtc).toISOString(),
      end: endUtc == null ? null : new Date(endUtc).toISOString(),
      all_day: ev.start.allDay,
      location: ev.location,
      calendar,
    });
  };

  for (const ev of events) {
    if (ev.cancelled) continue;
    if (ev.recurrenceId != null || !ev.rrule) {
      emit(ev, toUtc(ev.start));
      continue;
    }
    const ex = new Set(ev.exdates);
    const stopWall = toMs + 2 * DAY_MS;
    for (const wall of occurrences(ev, stopWall)) {
      const utc = ev.start.allDay ? wall : wallToUtc(wall, ev.start.tz);
      if (ex.has(utc)) continue;
      if (overrides.has(`${ev.uid}|${utc}`)) continue;
      emit(ev, utc);
    }
  }
  return out;
}

/** Merge per-calendar events: sorted by start, capped. */
export function mergeCalendarEvents(lists: CalendarEvent[][], max = 30): CalendarEvent[] {
  return lists
    .flat()
    .sort((a, b) => a.start.localeCompare(b.start) || a.title.localeCompare(b.title))
    .slice(0, max);
}
