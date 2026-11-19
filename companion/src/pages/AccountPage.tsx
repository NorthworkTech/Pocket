import { useNavigate } from 'react-router-dom'
import { useAuth } from '../lib/auth'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Group, PageHeader, Row } from '../components/ui'

export function AccountPage() {
  useDocumentTitle('Account')
  const { user, logout, entitlement } = useAuth()
  const navigate = useNavigate()

  async function onSignOut() {
    await logout()
    navigate('/login', { replace: true })
  }

  const initial = (user?.email ?? '?').trim().charAt(0).toUpperCase()
  const status = entitlement?.status
  const plan =
    status === 'active' ? 'Active' : status === 'trialing' ? 'Trial' : status === 'past_due' ? 'Payment issue' : 'Free'

  return (
    <div className="page stack">
      <PageHeader title="Account" />

      <div className="panel row" style={{ padding: '1rem' }}>
        <span className="avatar" aria-hidden="true">
          {initial}
        </span>
        <div className="stack-sm" style={{ gap: 0, minWidth: 0 }}>
          <span style={{ fontWeight: 600, overflow: 'hidden', textOverflow: 'ellipsis' }}>{user?.email ?? '—'}</span>
          <span className="muted" style={{ fontSize: '0.875rem' }}>
            Pocket Cloud · {plan}
          </span>
        </div>
      </div>

      <Group title="Pocket">
        <Row icon="device" to="/devices" title="Devices" />
        <Row icon="family" to="/family" title="Pocket Family" detail="Share lists with the people you live with" />
        <Row icon="calendar" to="/calendar" title="Calendar" detail="Your events on Pocket's Today" />
        <Row icon="link" to="/link" title="Link a Pocket" />
      </Group>

      <Group title="Pocket Cloud">
        <Row icon="cloud" tone="accent" to="/billing" title="Subscription" value={plan} />
        <Row icon="backup" to="/backup" title="Backup & restore" />
        <Row icon="external" to="/connectors" title="Connectors" detail="Google Drive, Dropbox, OneDrive" />
      </Group>

      <Group title="More">
        <Row icon="code" to="/developer" title="Developer" />
        {user?.is_admin || user?.role === 'admin' ? <Row icon="settings" to="/admin/" title="Pocket Cloud Admin" /> : null}
        <Row icon="logout" tone="danger" title="Sign out" onClick={() => void onSignOut()} />
      </Group>
    </div>
  )
}
