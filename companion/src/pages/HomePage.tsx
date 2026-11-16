import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api, isNetworkError } from '../lib/api'
import { useAuth } from '../lib/auth'
import type { Device, Note } from '../lib/types'
import { ApiError } from '../lib/types'
import { daysLeft, relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { Badge, EmptyState, Row, SkeletonList } from '../components/ui'
import { PocketMark } from '../components/WordMark'

export function HomePage() {
  useDocumentTitle('Home')
  const { entitlement, isEntitled, refresh, offline, user } = useAuth()
  const [notes, setNotes] = useState<Note[]>([])
  const [devices, setDevices] = useState<Device[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let cancelled = false
    ;(async () => {
      setLoading(true)
      setError(null)
      try {
        const devicesRes = await api.listDevices()
        if (cancelled) return
        setDevices(devicesRes.devices)
        if (isEntitled) {
          try {
            const notesRes = await api.listNotes()
            if (cancelled) return
            setNotes(notesRes.notes.filter((n) => !n.deleted_at).slice(0, 4))
          } catch (notesErr) {
            if (cancelled) return
            // Devices still useful; surface notes failure without blanking the home shell.
            if (isNetworkError(notesErr) || offline) {
              setError("You're offline or the server is unreachable.")
            } else if (notesErr instanceof ApiError) {
              setError(notesErr.message || 'Could not load notes.')
            } else {
              setError('Could not load notes.')
            }
            setNotes([])
          }
        } else {
          setNotes([])
        }
      } catch (err) {
        if (cancelled) return
        if (isNetworkError(err) || offline) {
          setError("You're offline or the server is unreachable.")
        } else if (err instanceof ApiError) {
          setError(err.message || 'Something went wrong.')
        } else {
          setError('Something went wrong.')
        }
      } finally {
        if (!cancelled) setLoading(false)
      }
    })()
    return () => {
      cancelled = true
    }
  }, [isEntitled, offline])

  const trialDays = daysLeft(entitlement?.trial_ends_at)
  const trialAvailable = entitlement && !entitlement.trial_consumed && entitlement.status === 'free'
  const hour = new Date().getHours()
  const greeting = hour < 5 ? 'Good evening' : hour < 12 ? 'Good morning' : hour < 17 ? 'Good afternoon' : 'Good evening'
  const initial = (user?.email ?? '?').trim().charAt(0).toUpperCase()
  const online = (d: Device) => !!d.last_seen_at && Date.now() - new Date(d.last_seen_at).getTime() < 1000 * 60 * 15

  return (
    <div className="page stack">
      <header className="greeting">
        <div className="stack-sm">
          <p className="eyebrow">
            {new Date().toLocaleDateString(undefined, { weekday: 'long', month: 'long', day: 'numeric' })}
          </p>
          <h1 className="page-title">{greeting}</h1>
        </div>
        <Link to="/account" className="avatar" aria-label="Account">
          {initial}
        </Link>
      </header>

      {error ? <ErrorState message={error} onRetry={() => void refresh()} /> : null}

      {devices.length === 0 ? (
        loading ? (
          <div className="device-card" aria-busy="true">
            <div className="device-card__art">
              <PocketMark size={22} />
            </div>
            <div className="device-card__body">
              <span className="device-card__name">Looking for your Pocket…</span>
            </div>
          </div>
        ) : (
          <Link to="/link" className="device-card">
            <div className="device-card__art">
              <Icon name="plus" size={22} />
            </div>
            <div className="device-card__body">
              <span className="device-card__name">Link your Pocket</span>
              <span className="device-card__meta">Open Link on Pocket, then continue here</span>
            </div>
            <Icon name="chevron-right" size={18} />
          </Link>
        )
      ) : (
        devices.map((d) => (
          <Link key={d.id} to={`/devices/${d.id}`} className="device-card">
            <div className="device-card__art">
              <PocketMark size={22} />
            </div>
            <div className="device-card__body">
              <span className="device-card__name">{d.device_name || 'Pocket'}</span>
              <span className="device-card__meta">
                <span className={online(d) ? 'dot online' : 'dot'} />
                {online(d)
                  ? `Online · ${relativeTime(d.last_seen_at!)}`
                  : d.last_seen_at
                    ? `Last seen ${relativeTime(d.last_seen_at)}`
                    : 'Waiting to check in'}
              </span>
            </div>
            <Icon name="chevron-right" size={18} />
          </Link>
        ))
      )}

      {isEntitled ? (
        <>
          {entitlement?.status === 'trialing' && trialDays !== null ? (
            <Link to="/billing" className="panel row-between" style={{ padding: '0.875rem 1rem', color: 'inherit' }}>
              <span className="row">
                <Badge tone="accent">Trial</Badge>
                <span className="muted" style={{ fontSize: '0.875rem', fontWeight: 400 }}>
                  {trialDays} days of Pocket Cloud left
                </span>
              </span>
              <Icon name="chevron-right" size={16} className="cell__chevron" />
            </Link>
          ) : null}

          <section className="stack-sm">
            <div className="section-head">
              <h2>Recent notes</h2>
              <Link to="/notes">See all</Link>
            </div>
            {loading ? (
              <SkeletonList rows={3} />
            ) : notes.length === 0 ? (
              <EmptyState icon="notes" title="No notes yet">
                Hold the side button on Pocket to dictate. Notes appear here.
              </EmptyState>
            ) : (
              <ul className="group__list">
                {notes.map((n) => (
                  <Row
                    key={n.id}
                    to={`/notes/${n.id}`}
                    title={n.title || 'Untitled'}
                    detail={n.body ? n.body.slice(0, 80) : relativeTime(n.updated_at)}
                    value={relativeTime(n.updated_at)}
                  />
                ))}
              </ul>
            )}
          </section>

          <section className="stack-sm">
            <div className="section-head">
              <h2>Library</h2>
            </div>
            <div className="tiles">
              {(
                [
                  ['/lists', 'lists', 'Lists', 'Checklists'],
                  ['/music', 'music', 'Music', 'Tracks on Pocket'],
                  ['/reading', 'reading', 'Reading', 'Books on Pocket'],
                  ['/backup', 'backup', 'Backup', 'Copies & restore'],
                ] as const
              ).map(([to, icon, label, meta]) => (
                <Link key={to} to={to} className="tile">
                  <span className="tile__icon">
                    <Icon name={icon} size={20} />
                  </span>
                  <span className="tile__label">{label}</span>
                  <span className="tile__meta">{meta}</span>
                </Link>
              ))}
            </div>
          </section>
        </>
      ) : (
        <section className="promo">
          <span className="eyebrow" style={{ color: 'rgba(255,255,255,0.7)' }}>
            Pocket Cloud
          </span>
          <h2 style={{ fontSize: '1.5rem' }}>Your Pocket, on your phone</h2>
          <p className="muted">
            Sync Notes, Lists, Music and Reading between Pocket and Companion — plus backup and connectors.
          </p>
          <div className="row" style={{ gap: '0.75rem' }}>
            {trialAvailable ? (
              <Link className="btn btn-primary" to="/billing">
                Start 7-day free trial
              </Link>
            ) : entitlement?.status === 'lapsed' ? (
              <Link className="btn btn-primary" to="/billing">
                Resubscribe
              </Link>
            ) : (
              <Link className="btn btn-primary" to="/upgrade">
                Subscribe
              </Link>
            )}
            <span className="muted" style={{ fontSize: '0.875rem' }}>
              $3.99 / month
            </span>
          </div>
        </section>
      )}
    </div>
  )
}
