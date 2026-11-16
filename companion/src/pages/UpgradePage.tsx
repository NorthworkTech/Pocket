import { useState } from 'react'
import { Link, useSearchParams } from 'react-router-dom'
import { api, isNetworkError } from '../lib/api'
import { useAuth } from '../lib/auth'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Group, PageHeader, Row } from '../components/ui'

export function UpgradePage() {
  useDocumentTitle("Pocket Cloud isn't active")
  const { entitlement } = useAuth()
  const [params] = useSearchParams()
  const pastDue = params.get('reason') === 'past_due' || entitlement?.status === 'past_due'
  const trialAvailable = entitlement && !entitlement.trial_consumed
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  async function startCheckout() {
    setBusy(true)
    setError(null)
    try {
      const { url } = await api.createCheckout()
      window.location.href = url
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
      setBusy(false)
    }
  }

  async function openPortal() {
    setBusy(true)
    setError(null)
    try {
      const { url } = await api.createPortal()
      window.location.href = url
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
      setBusy(false)
    }
  }

  if (pastDue) {
    return (
      <div className="page stack">
        <div className="panel stack">
          <span className="badge badge--warn">Payment issue</span>
          <h1>Update your payment to keep syncing</h1>
          {error ? <ErrorState message={error} onRetry={() => void openPortal()} /> : null}
          <button type="button" className="btn btn-primary" disabled={busy} onClick={() => void openPortal()}>
            Update payment
          </button>
          <Link to="/home" className="btn btn-ghost">
            Home
          </Link>
        </div>
      </div>
    )
  }

  return (
    <div className="page stack">
      <PageHeader title="Pocket Cloud" back={{ to: '/home', label: 'Home' }} />
      <section className="promo">
        <span className="eyebrow" style={{ color: 'rgba(255,255,255,0.7)' }}>
          Not active
        </span>
        <h2 style={{ fontSize: '1.5rem' }}>Sync Pocket with your phone</h2>
        <p className="muted">
          Your Notes still live on Pocket. Subscribe to edit them here, back them up, and keep copies where you
          already work.
        </p>
        {error ? <ErrorState message={error} onRetry={() => void startCheckout()} /> : null}
        <div className="actions">
          {trialAvailable ? (
            <button type="button" className="btn btn-primary btn-block" disabled={busy} onClick={() => void startCheckout()}>
              Start 7-day free trial
            </button>
          ) : (
            <button type="button" className="btn btn-primary btn-block" disabled={busy} onClick={() => void startCheckout()}>
              Subscribe — $3.99 / month
            </button>
          )}
        </div>
      </section>
      <Group>
        {entitlement?.has_customer ? (
          <Row icon="card" title="Manage billing" onClick={() => void openPortal()} disabled={busy} chevron />
        ) : (
          <Row icon="card" title="Plans & billing" to="/billing" />
        )}
      </Group>
    </div>
  )
}
