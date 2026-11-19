import { describe, expect, it } from "vitest";
import {
  expandEvents,
  mergeCalendarEvents,
  parseDuration,
  parseIcs,
  unfoldLines,
  wallToUtc,
} from "../src/lib/ics.js";

const cal = (...events: string[]) =>
  ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//test//EN", ...events, "END:VCALENDAR"].join("\r\n");

const window = (from: string, to: string, ics: string, label = "Home") =>
  expandEvents(parseIcs(ics), new Date(from), new Date(to), label);

describe("ICS parser", () => {
  it("unfolds folded lines and unescapes text", () => {
    expect(unfoldLines("SUMMARY:Dentist appoint\r\n ment\r\nX:1")).toEqual([
      "SUMMARY:Dentist appointment",
      "X:1",
    ]);
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:fold-1",
      "DTSTART:20261120T150000Z",
      "DTEND:20261120T160000Z",
      "SUMMARY:Dentist appoint",
      " ment with Dr. Okafor",
      "LOCATION:12 High St\\, Suite 4\\nLondon",
      "END:VEVENT",
    );
    const [ev] = window("2026-11-19T00:00:00Z", "2026-11-26T00:00:00Z", ics);
    expect(ev).toEqual({
      title: "Dentist appointment with Dr. Okafor",
      start: "2026-11-20T15:00:00.000Z",
      end: "2026-11-20T16:00:00.000Z",
      all_day: false,
      location: "12 High St, Suite 4\nLondon",
      calendar: "Home",
    });
  });

  it("dates all-day events at 00:00 UTC", () => {
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:allday",
      "DTSTART;VALUE=DATE:20261120",
      "DTEND;VALUE=DATE:20261122",
      "SUMMARY:Grandma visiting",
      "END:VEVENT",
      // DTSTART only (no DTEND): one day long.
      "BEGIN:VEVENT",
      "UID:allday-2",
      "DTSTART;VALUE=DATE:20261124",
      "SUMMARY:Bin day",
      "END:VEVENT",
    );
    const evs = window("2026-11-19T12:00:00Z", "2026-11-26T12:00:00Z", ics);
    expect(evs.map((e) => [e.title, e.start, e.end, e.all_day])).toEqual([
      ["Grandma visiting", "2026-11-20T00:00:00.000Z", "2026-11-22T00:00:00.000Z", true],
      ["Bin day", "2026-11-24T00:00:00.000Z", "2026-11-25T00:00:00.000Z", true],
    ]);
  });

  it("keeps today's all-day event even when UTC has moved on", () => {
    // 20:00 on Nov 19 in Los Angeles is already Nov 20 04:00 UTC.
    const ics = cal("BEGIN:VEVENT", "UID:a", "DTSTART;VALUE=DATE:20261119", "SUMMARY:Today", "END:VEVENT");
    expect(window("2026-11-20T04:00:00Z", "2026-11-27T04:00:00Z", ics)).toHaveLength(1);
  });

  it("converts TZID wall times to UTC (incl. DST)", () => {
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:tz-1",
      "DTSTART;TZID=Europe/Berlin:20261120T183000",
      "DTEND;TZID=Europe/Berlin:20261120T200000",
      "SUMMARY:Concert",
      "END:VEVENT",
      "BEGIN:VEVENT",
      "UID:tz-2",
      'DTSTART;TZID="/mozilla.org/20050126_1/America/New_York":20260701T090000',
      "DURATION:PT30M",
      "SUMMARY:Summer standup",
      "END:VEVENT",
    );
    const evs = window("2026-06-01T00:00:00Z", "2026-12-01T00:00:00Z", ics);
    expect(evs.map((e) => [e.title, e.start, e.end])).toEqual([
      ["Concert", "2026-11-20T17:30:00.000Z", "2026-11-20T19:00:00.000Z"],
      ["Summer standup", "2026-07-01T13:00:00.000Z", "2026-07-01T13:30:00.000Z"],
    ]);
    expect(new Date(wallToUtc(Date.UTC(2026, 2, 8, 12), "America/Los_Angeles")).toISOString()).toBe(
      "2026-03-08T19:00:00.000Z",
    );
  });

  it("expands a weekly RRULE with BYDAY, COUNT and EXDATE across a DST change", () => {
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:weekly-1",
      "DTSTART;TZID=America/New_York:20261026T090000",
      "DTEND;TZID=America/New_York:20261026T093000",
      "RRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=6",
      "EXDATE;TZID=America/New_York:20261104T090000",
      "SUMMARY:Piano lesson",
      "BEGIN:VALARM",
      "TRIGGER:-PT15M",
      "SUMMARY:alarm summary must not leak",
      "END:VALARM",
      "END:VEVENT",
    );
    const evs = window("2026-10-01T00:00:00Z", "2026-12-31T00:00:00Z", ics);
    expect(evs.map((e) => e.start)).toEqual([
      "2026-10-26T13:00:00.000Z",
      "2026-10-28T13:00:00.000Z",
      // US clocks fall back on Nov 1: 9:00 local is now 14:00 UTC.
      "2026-11-02T14:00:00.000Z",
      // Nov 4 excluded (still counts toward COUNT=6).
      "2026-11-09T14:00:00.000Z",
      "2026-11-11T14:00:00.000Z",
    ]);
    expect(evs.every((e) => e.title === "Piano lesson")).toBe(true);
    expect(evs[0]!.end).toBe("2026-10-26T13:30:00.000Z");
  });

  it("handles INTERVAL, UNTIL, MONTHLY BYDAY and YEARLY rules", () => {
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:daily",
      "DTSTART:20261116T070000Z",
      "RRULE:FREQ=DAILY;INTERVAL=2;UNTIL=20261124T070000Z",
      "SUMMARY:Run",
      "END:VEVENT",
      "BEGIN:VEVENT",
      "UID:monthly",
      "DTSTART;TZID=Europe/London:20260105T190000",
      "RRULE:FREQ=MONTHLY;BYDAY=-1FR",
      "SUMMARY:Book club",
      "END:VEVENT",
      "BEGIN:VEVENT",
      "UID:yearly",
      "DTSTART;VALUE=DATE:19900311",
      "RRULE:FREQ=YEARLY",
      "SUMMARY:Sam's birthday",
      "END:VEVENT",
    );
    const evs = window("2026-11-19T00:00:00Z", "2026-11-30T00:00:00Z", ics);
    expect(evs.map((e) => [e.title, e.start])).toEqual([
      ["Run", "2026-11-20T07:00:00.000Z"],
      ["Run", "2026-11-22T07:00:00.000Z"],
      ["Run", "2026-11-24T07:00:00.000Z"],
      ["Book club", "2026-11-27T19:00:00.000Z"],
    ]);
    const spring = window("2027-03-10T00:00:00Z", "2027-03-17T00:00:00Z", ics);
    expect(spring.map((e) => [e.title, e.start, e.all_day])).toContainEqual([
      "Sam's birthday",
      "2027-03-11T00:00:00.000Z",
      true,
    ]);
  });

  it("finds this week's occurrence of a daily event that began years ago", () => {
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:old-daily",
      "DTSTART;TZID=America/Los_Angeles:20150301T080000",
      "DURATION:PT15M",
      "RRULE:FREQ=DAILY",
      "SUMMARY:Meds",
      "END:VEVENT",
    );
    const evs = window("2026-11-19T00:00:00Z", "2026-11-22T00:00:00Z", ics);
    expect(evs.map((e) => e.start)).toEqual([
      "2026-11-19T16:00:00.000Z",
      "2026-11-20T16:00:00.000Z",
      "2026-11-21T16:00:00.000Z",
    ]);
  });

  it("applies RECURRENCE-ID overrides and drops cancelled events", () => {
    const ics = cal(
      "BEGIN:VEVENT",
      "UID:series",
      "DTSTART:20261116T120000Z",
      "DTEND:20261116T130000Z",
      "RRULE:FREQ=DAILY;COUNT=5",
      "SUMMARY:Lunch",
      "END:VEVENT",
      "BEGIN:VEVENT",
      "UID:series",
      "RECURRENCE-ID:20261118T120000Z",
      "DTSTART:20261118T133000Z",
      "DTEND:20261118T143000Z",
      "SUMMARY:Late lunch",
      "END:VEVENT",
      "BEGIN:VEVENT",
      "UID:gone",
      "DTSTART:20261118T090000Z",
      "STATUS:CANCELLED",
      "SUMMARY:Cancelled",
      "END:VEVENT",
    );
    const evs = mergeCalendarEvents([window("2026-11-17T00:00:00Z", "2026-11-19T00:00:00Z", ics)]);
    expect(evs.map((e) => [e.title, e.start])).toEqual([
      ["Lunch", "2026-11-17T12:00:00.000Z"],
      ["Late lunch", "2026-11-18T13:30:00.000Z"],
    ]);
  });

  it("parses durations", () => {
    expect(parseDuration("PT1H30M")).toBe(90 * 60000);
    expect(parseDuration("P1W")).toBe(7 * 86400000);
    expect(parseDuration("P1DT2H")).toBe(26 * 3600000);
    expect(parseDuration("nope")).toBeNull();
  });

  it("ignores junk without throwing", () => {
    expect(parseIcs("not a calendar")).toEqual([]);
    expect(parseIcs(cal("BEGIN:VEVENT", "SUMMARY:No start", "END:VEVENT"))).toEqual([]);
  });
});
