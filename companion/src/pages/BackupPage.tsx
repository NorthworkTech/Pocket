import { useEffect, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import type { BackupMeta } from '../lib/types'
import { formatDate } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { EmptyState, Group, PageHeader, Row } from '../components/ui'

export function BackupPage() {
  useDocumentTitle('Backup & restore')
  const [backups, setBackups] = useState<BackupMeta[]>([])
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [confirmId, setConfirmId] = useState<string | null>(null)

  async function load() {
    setError(null)
    try {
      const res = await api.listBackups()
      setBackups(res.backups)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    }
  }

  useEffect(() => {
    void load()
  }, [])

  async function onCreate() {
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      await api.createBackup()
      setMsg('Backup created.')
      await load()
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    } finally {
      setBusy(false)
    }
  }

  async function onDownload() {
    setBusy(true)
    setError(null)
    try {
      const { url } = await api.downloadLatestBackup()
      window.location.href = url
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    } finally {
      setBusy(false)
    }
  }

  async function onRestore(id: string) {
    setBusy(true)
    setError(null)
    try {
      await api.restoreBackup(id)
      setMsg('Backup restored. Devices will update when online.')
      setConfirmId(null)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't restore backup.")
    } finally {
      setBusy(false)
    }
  }

  const latest = backups[0]

  return (
    <div className="page stack">
      <PageHeader
        title="Backup"
        back={{ to: '/account', label: 'Account' }}
        subtitle="Snapshots of your Notes and Lists in Pocket Cloud. Restore any time."
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}
      {msg ? <p className="badge badge--accent">{msg}</p> : null}

      <Group>
        <Row icon="backup" tone="accent" title="Back up now" onClick={() => void onCreate()} disabled={busy} chevron />
        <Row icon="download" title="Download latest" onClick={() => void onDownload()} disabled={busy || !latest} chevron />
        {latest ? <Row icon="sync" title="Restore latest…" onClick={() => setConfirmId(latest.id)} disabled={busy} chevron /> : null}
      </Group>

      {backups.length > 0 ? (
        <Group title="History">
          {backups.map((b) => (
            <Row key={b.id} title={formatDate(b.created_at)} detail={`${b.note_count} notes · ${b.list_count} lists`} />
          ))}
        </Group>
      ) : (
        <EmptyState icon="backup" title="No backups yet">
          Your first backup captures every Note and List in Pocket Cloud.
        </EmptyState>
      )}

      {confirmId ? (
        <div className="panel stack" role="alertdialog" aria-label="Restore backup">
          <p>Restore replaces Cloud Notes &amp; Lists with this backup. Pocket syncs after the restore.</p>
          <div className="actions-row">
            <button type="button" className="btn btn-primary" disabled={busy} onClick={() => void onRestore(confirmId)}>
              Restore
            </button>
            <button type="button" className="btn btn-secondary" onClick={() => setConfirmId(null)}>
              Cancel
            </button>
          </div>
        </div>
      ) : null}
    </div>
  )
}
