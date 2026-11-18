import { useEffect, useMemo, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import type { Device, HomeAppKey } from '../lib/types'
import { Icon, type IconName } from './Icon'
import { Group, Row } from './ui'

/** Pocket's Home apps in their default order (firmware contract). */
export const HOME_APPS: { key: HomeAppKey; label: string; icon: IconName }[] = [
  { key: 'today', label: 'Today', icon: 'calendar' },
  { key: 'notes', label: 'Notes & Lists', icon: 'notes' },
  { key: 'clock', label: 'Clock', icon: 'clock' },
  { key: 'weather', label: 'Weather', icon: 'sun' },
  { key: 'music', label: 'Music', icon: 'music' },
  { key: 'reading', label: 'Reading', icon: 'reading' },
  { key: 'pass', label: 'Passes', icon: 'pass' },
  { key: 'settings', label: 'Settings', icon: 'settings' },
  { key: 'update', label: 'Software update', icon: 'download' },
]

const DEFAULT_ORDER = HOME_APPS.map((a) => a.key)
const BY_KEY = new Map(HOME_APPS.map((a) => [a.key, a]))

/** Saved order (possibly partial or empty) → full order Companion can edit. */
export function resolveHomeOrder(saved?: readonly string[] | null): HomeAppKey[] {
  const known = (saved ?? []).filter((k): k is HomeAppKey => BY_KEY.has(k as HomeAppKey))
  const unique = [...new Set(known)]
  return [...unique, ...DEFAULT_ORDER.filter((k) => !unique.includes(k))]
}

const sameOrder = (a: readonly string[], b: readonly string[]) => a.length === b.length && a.every((k, i) => k === b[i])

/** "Home screen" card: reorder Pocket's Home apps with up/down buttons, then save to Pocket Cloud. */
export function HomeOrderEditor({ device, onSaved }: { device: Device; onSaved: (d: Device) => void }) {
  const saved = useMemo(() => resolveHomeOrder(device.home_order), [device.home_order])
  const [order, setOrder] = useState<HomeAppKey[]>(saved)
  const [busy, setBusy] = useState(false)
  const [msg, setMsg] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [announce, setAnnounce] = useState('')

  useEffect(() => setOrder(saved), [saved])

  const dirty = !sameOrder(order, saved)
  const isDefault = sameOrder(order, DEFAULT_ORDER)

  function move(index: number, delta: -1 | 1) {
    const to = index + delta
    if (to < 0 || to >= order.length) return
    const next = [...order]
    const [key] = next.splice(index, 1)
    next.splice(to, 0, key!)
    setOrder(next)
    setMsg(null)
    setAnnounce(`${BY_KEY.get(key!)!.label} moved to position ${to + 1} of ${next.length}.`)
    // Keep focus on the moved row's button, or its sibling if that button just became disabled.
    requestAnimationFrame(() => {
      const atEdge = (delta === -1 && to === 0) || (delta === 1 && to === next.length - 1)
      const dir = atEdge ? (delta === -1 ? 'down' : 'up') : delta === -1 ? 'up' : 'down'
      document.getElementById(`home-move-${dir}-${key}`)?.focus()
    })
  }

  async function onSave() {
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      const d = await api.updateDevice(device.id, { home_order: order })
      onSaved({ ...device, ...d, home_order: d.home_order ?? order })
      setMsg('Home screen saved.')
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable. Your order isn't saved yet.")
      else if (err instanceof Error && err.message) setError(err.message)
      else setError("Couldn't save the Home screen order.")
    } finally {
      setBusy(false)
    }
  }

  return (
    <section className="panel stack" aria-labelledby="home-order-title">
      <div className="card-heading">
        <span className="cell__icon">
          <Icon name="home" size={18} stroke={2} />
        </span>
        <h2 id="home-order-title" className="card-heading__title">
          Home screen
        </h2>
      </div>
      <p className="muted">Choose the order apps appear on Pocket's Home screen.</p>

      <Group>
        {order.map((key, i) => {
          const app = BY_KEY.get(key)!
          return (
            <Row
              key={key}
              icon={app.icon}
              title={app.label}
              value={
                <span className="reorder">
                  <button
                    type="button"
                    id={`home-move-up-${key}`}
                    className="btn btn-ghost reorder__btn"
                    aria-label={`Move ${app.label} up`}
                    disabled={busy || i === 0}
                    onClick={() => move(i, -1)}
                  >
                    <Icon name="chevron-up" size={20} stroke={2.2} />
                  </button>
                  <button
                    type="button"
                    id={`home-move-down-${key}`}
                    className="btn btn-ghost reorder__btn"
                    aria-label={`Move ${app.label} down`}
                    disabled={busy || i === order.length - 1}
                    onClick={() => move(i, 1)}
                  >
                    <Icon name="chevron-down" size={20} stroke={2.2} />
                  </button>
                </span>
              }
            />
          )
        })}
      </Group>
      <p className="sr-only" aria-live="polite">
        {announce}
      </p>

      <div className="actions-row">
        <button type="button" className="btn btn-primary" disabled={busy || !dirty} onClick={() => void onSave()}>
          {busy ? 'Saving…' : 'Save'}
        </button>
        {!isDefault ? (
          <button
            type="button"
            className="btn btn-ghost"
            disabled={busy}
            onClick={() => {
              setOrder(DEFAULT_ORDER)
              setMsg(null)
              setAnnounce('Default order restored. Save to send it to Pocket.')
            }}
          >
            Use default order
          </button>
        ) : null}
      </div>
      <p className="muted card-note">Changes reach Pocket the next time it checks in.</p>
      <div aria-live="polite">
        {msg ? (
          <p className="inline-note inline-note--accent">
            <Icon name="check" size={16} stroke={2.4} />
            <span>{msg}</span>
          </p>
        ) : null}
        {error ? (
          <p role="alert" className="inline-note inline-note--danger">
            {error}
          </p>
        ) : null}
      </div>
    </section>
  )
}
