import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api, isNetworkError } from '../lib/api'
import type { Device } from '../lib/types'
import { relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { EmptyState, PageHeader, SkeletonList } from '../components/ui'
import { PocketMark } from '../components/WordMark'

export function DevicesPage() {
  useDocumentTitle('Devices')
  const [devices, setDevices] = useState<Device[]>([])
  const [error, setError] = useState<string | null>(null)
  const [showHelp, setShowHelp] = useState(false)
  const [loading, setLoading] = useState(true)

  async function load() {
    setLoading(true)
    setError(null)
    try {
      const res = await api.listDevices()
      setDevices(res.devices)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    void load()
  }, [])

  const isOnline = (d: (typeof devices)[number]) =>
    !!d.last_seen_at && Date.now() - new Date(d.last_seen_at).getTime() < 1000 * 60 * 15

  return (
    <div className="page stack">
      <PageHeader
        title="Your Pocket"
        subtitle="Name, sound, display and Wi-Fi for each linked device."
        action={
          <Link className="btn btn-secondary btn-sm" to="/link">
            <Icon name="plus" size={16} stroke={2.4} /> Link
          </Link>
        }
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      {showHelp ? (
        <div className="panel stack-sm" role="dialog" aria-label="Link instructions">
          <p>On your Pocket: open Link, then continue in this app.</p>
          <div className="actions-row">
            <Link className="btn btn-primary" to="/link">
              Link a Pocket
            </Link>
            <button type="button" className="btn btn-ghost" onClick={() => setShowHelp(false)}>
              Close
            </button>
          </div>
        </div>
      ) : null}

      {loading ? (
        <SkeletonList rows={1} />
      ) : devices.length === 0 ? (
        <EmptyState
          icon="device"
          title="No Pocket linked yet"
          action={
            <Link className="btn btn-primary" to="/link">
              Link a Pocket
            </Link>
          }
        >
          On Pocket, open Link. You'll share Wi-Fi from this phone, then confirm the code on the device.
        </EmptyState>
      ) : (
        devices.map((d) => (
          <Link key={d.id} to={`/devices/${d.id}`} className="device-card">
            <div className="device-card__art">
              <PocketMark size={22} />
            </div>
            <div className="device-card__body">
              <span className="device-card__name">{d.device_name || 'Pocket'}</span>
              <span className="device-card__meta">
                <span className={isOnline(d) ? 'dot online' : 'dot'} />
                {isOnline(d)
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
    </div>
  )
}
