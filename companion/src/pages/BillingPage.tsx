import { useState } from 'react'
import { Link } from 'react-router-dom'
import { api, isNetworkError } from '../lib/api'
import { useAuth } from '../lib/auth'
import { formatDate } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { EmptyState, Group, PageHeader, Row } from '../components/ui'

export function BillingPage() {
  useDocumentTitle('Billing')
  const { entitlement, refresh } = useAuth()
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  const trialAvailable = entitlement && !entitlement.trial_consumed && (entitlement.status === 'free' || entitlement.status === 'lapsed')
  const isLapsed = entitlement?.status === 'lapsed'
  const isEntitled = entitlement?.entitled

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

  let statusLine: string | null = null
  if (entitlement?.status === 'trialing' && entitlement.trial_ends_at) {
    statusLine = `Trial ends ${formatDate(entitlement.trial_ends_at)}`
  } else if (entitlement?.status === 'active' && entitlement.cancel_at_period_end && entitlement.current_period_end) {
    statusLine = `Canceled · access until ${formatDate(entitlement.current_period_end)}`
  } else if (entitlement?.status === 'active' && entitlement.current_period_end) {
    statusLine = `Next bill ${formatDate(entitlement.current_period_end)}`
  } else if (entitlement?.status === 'past_due') {
    statusLine = 'Payment issue'
  }

  const primaryLabel = trialAvailable ? 'Start free trial' : isLapsed ? 'Resubscribe' : isEntitled ? 'Subscribe' : 'Subscribe'

  return (
    <div className="page stack">
      <PageHeader title="Pocket Cloud" back={{ to: '/account', label: 'Account' }} />

      {error ? <ErrorState message={error} onRetry={() => void refresh()} /> : null}

      <section className="promo">
        <div className="row-between">
          <span className="eyebrow" style={{ color: 'rgba(255,255,255,0.7)' }}>
            Monthly
          </span>
          {statusLine ? (
            <span className="badge" style={{ background: 'rgba(255,255,255,0.16)', color: '#fff' }}>
              {statusLine}
            </span>
          ) : null}
        </div>
        <p className="hero-price">
          $3.99<span style={{ fontSize: '1rem', fontWeight: 500, opacity: 0.8 }}> / month</span>
        </p>
        {trialAvailable ? <p className="muted">7 days free, then $3.99 a month. Cancel anytime.</p> : <p className="muted">Cancel anytime. Prices in USD.</p>}
        <div className="actions">
          {!isEntitled || isLapsed || trialAvailable ? (
            <button type="button" className="btn btn-primary btn-block" disabled={busy} onClick={() => void startCheckout()}>
              {primaryLabel}
            </button>
          ) : null}
        </div>
      </section>

      <Group title="Included">
        <Row icon="notes" title="Notes & Lists sync" detail="Edit on Pocket or phone" />
        <Row icon="music" title="Music & Reading library" detail="Send tracks and books to Pocket" />
        <Row icon="backup" title="Backup & restore" />
        <Row icon="external" title="Connectors" detail="Drive, Dropbox, OneDrive" />
      </Group>

      {entitlement?.has_customer || isEntitled ? (
        <Group>
          <Row icon="card" title="Manage billing" onClick={() => void openPortal()} disabled={busy} chevron />
        </Group>
      ) : null}

      <p className="footer-note">Yearly plan coming soon.</p>
    </div>
  )
}

export function BillingSuccessPage() {
  useDocumentTitle("You're all set")
  return (
    <div className="page stack">
      <EmptyState
        icon="check"
        title="You're all set"
        action={
          <Link className="btn btn-primary" to="/notes">
            Go to Notes
          </Link>
        }
      >
        Pocket Cloud is active. Your Notes and Lists now sync with Pocket.
      </EmptyState>
    </div>
  )
}

export function BillingCancelPage() {
  useDocumentTitle('Checkout canceled')
  return (
    <div className="page stack">
      <EmptyState
        icon="card"
        title="Checkout canceled"
        action={
          <Link className="btn btn-secondary" to="/billing">
            Back to Pocket Cloud
          </Link>
        }
      >
        Nothing was charged. You can subscribe anytime.
      </EmptyState>
    </div>
  )
}
