import { type FormEvent, useEffect, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import { ApiError, type CalendarFeed } from '../lib/types'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Group, PageHeader, Row, SkeletonList } from '../components/ui'

function errorText(err: unknown): string {
  if (isNetworkError(err)) return "You're offline or the server is unreachable."
  if (err instanceof ApiError && err.message) return err.message
  return 'Something went wrong.'
}

function looksLikeFeedUrl(raw: string): boolean {
  return /^(https|webcals?):\/\/[^\s/]+\.[^\s]+$/i.test(raw.trim())
}

export function CalendarPage() {
  useDocumentTitle('Calendar')
  const [feeds, setFeeds] = useState<CalendarFeed[]>([])
  const [max, setMax] = useState(5)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [label, setLabel] = useState('')
  const [url, setUrl] = useState('')
  const [confirm, setConfirm] = useState<CalendarFeed | null>(null)

  async function load() {
    setLoading(true)
    setError(null)
    try {
      const r = await api.listCalendarFeeds()
      setFeeds(r.feeds)
      setMax(r.max_feeds || 5)
    } catch (err) {
      setError(errorText(err))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    void load()
  }, [])

  async function onAdd(e: FormEvent) {
    e.preventDefault()
    if (!looksLikeFeedUrl(url)) {
      setError("Paste the calendar's iCal link (it starts with https:// or webcal://).")
      return
    }
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      const { feed } = await api.addCalendarFeed(label.trim() || 'Calendar', url.trim())
      setFeeds((prev) => [...prev, feed])
      setLabel('')
      setUrl('')
      setMsg(`${feed.label} added to Today.`)
    } catch (err) {
      setError(errorText(err))
    } finally {
      setBusy(false)
    }
  }

  async function onRemove(feed: CalendarFeed) {
    setBusy(true)
    setError(null)
    try {
      await api.removeCalendarFeed(feed.id)
      setFeeds((prev) => prev.filter((f) => f.id !== feed.id))
      setConfirm(null)
      setMsg(`${feed.label} removed.`)
    } catch (err) {
      setError(errorText(err))
    } finally {
      setBusy(false)
    }
  }

  const full = feeds.length >= max

  return (
    <div className="page stack">
      <PageHeader
        title="Calendar"
        back={{ to: '/account', label: 'Account' }}
        subtitle="Your next seven days, on Pocket's Today screen."
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}
      {msg ? <p className="badge badge--accent">{msg}</p> : null}

      {loading ? (
        <SkeletonList rows={1} />
      ) : feeds.length ? (
        <Group title="On Today" footer={`${feeds.length} of ${max} calendars. Pocket Cloud checks them about every 10 minutes.`}>
          {feeds.map((f) => (
            <Row
              key={f.id}
              icon="calendar"
              title={f.label}
              detail={f.host}
              value={<span className="cell__title--danger">Remove</span>}
              onClick={() => setConfirm(f)}
              chevron={false}
              disabled={busy}
            />
          ))}
        </Group>
      ) : null}

      {confirm ? (
        <div className="panel stack" role="alertdialog" aria-label="Remove calendar">
          <p style={{ margin: 0 }}>Remove {confirm.label} from Today? Nothing changes in the calendar itself.</p>
          <div className="actions-row">
            <button type="button" className="btn btn-danger" disabled={busy} onClick={() => void onRemove(confirm)}>
              Remove
            </button>
            <button type="button" className="btn btn-secondary" onClick={() => setConfirm(null)}>
              Cancel
            </button>
          </div>
        </div>
      ) : null}

      {!full ? (
        <form className="panel stack" onSubmit={(e) => void onAdd(e)}>
          <h2 style={{ margin: 0 }}>{feeds.length ? 'Add another calendar' : 'Add a calendar'}</h2>
          <div className="field">
            <label htmlFor="cal-label">Name</label>
            <input
              id="cal-label"
              value={label}
              maxLength={40}
              onChange={(e) => setLabel(e.target.value)}
              placeholder="Family"
            />
          </div>
          <div className="field">
            <label htmlFor="cal-url">iCal link</label>
            <input
              id="cal-url"
              type="url"
              inputMode="url"
              autoCapitalize="off"
              autoComplete="off"
              spellCheck={false}
              value={url}
              onChange={(e) => setUrl(e.target.value)}
              placeholder="webcal://… or https://…"
            />
          </div>
          <button type="submit" className="btn btn-primary" disabled={busy || !url.trim()}>
            Add calendar
          </button>
        </form>
      ) : null}

      <div className="panel stack-sm">
        <h2 style={{ margin: 0, fontSize: '1.05rem' }}>Where to find the link</h2>
        <ul className="help-list">
          <li>
            <strong>Google Calendar</strong>
            <span>On a computer, open Settings → pick the calendar → copy “Secret address in iCal format”.</span>
          </li>
          <li>
            <strong>Apple Calendar (iCloud)</strong>
            <span>In Calendar, tap ⓘ next to the calendar, turn on Public Calendar, then Share Link → Copy.</span>
          </li>
        </ul>
      </div>
    </div>
  )
}
