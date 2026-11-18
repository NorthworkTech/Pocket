import { useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import { relativeTime } from '../lib/utils'
import { Icon } from './Icon'

const QUEUED_MSG =
  "Pocket will chime the next time it checks in — within a minute while it's awake, up to 10 minutes on the lock screen."

/** "Find my Pocket" card: queues a one-off chime that Pocket plays on its next check-in. */
export function FindMyPocket({
  deviceId,
  online,
  lastSeen,
}: {
  deviceId: string
  online: boolean
  lastSeen?: string | null
}) {
  const [busy, setBusy] = useState(false)
  const [queued, setQueued] = useState(false)
  const [error, setError] = useState<string | null>(null)

  async function onRing() {
    setBusy(true)
    setError(null)
    setQueued(false)
    try {
      await api.ringDevice(deviceId)
      setQueued(true)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable. Try again when you're connected.")
      else if (err instanceof Error && err.message) setError(err.message)
      else setError("Couldn't reach Pocket Cloud. Try again.")
    } finally {
      setBusy(false)
    }
  }

  return (
    <section className="panel stack" aria-labelledby="find-pocket-title">
      <div className="card-heading">
        <span className="cell__icon">
          <Icon name="bell" size={18} stroke={2} />
        </span>
        <h2 id="find-pocket-title" className="card-heading__title">
          Find my Pocket
        </h2>
      </div>
      <p className="muted">Play a short chime on Pocket to find it nearby.</p>
      {!online && lastSeen ? (
        <p className="muted card-note">
          Pocket last checked in {relativeTime(lastSeen)}. A chime waits up to 30 minutes for it to come back online.
        </p>
      ) : null}
      <button type="button" className="btn btn-primary btn-block" disabled={busy} onClick={() => void onRing()}>
        <Icon name="bell" size={18} stroke={2.2} />
        {busy ? 'Sending…' : 'Play sound on Pocket'}
      </button>
      <div aria-live="polite">
        {queued ? (
          <p className="inline-note inline-note--accent">
            <Icon name="check" size={16} stroke={2.4} />
            <span>{QUEUED_MSG}</span>
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
