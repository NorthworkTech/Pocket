import { useEffect, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import type { BackupMeta, Device, DeviceBackup } from '../lib/types'
import { formatDate, relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { EmptyState, Group, PageHeader, Row, SkeletonList } from '../components/ui'

const plural = (n: number, one: string, many = `${one}s`) => `${n} ${n === 1 ? one : many}`

function backupWhen(iso: string): string {
  const d = new Date(iso)
  return `${formatDate(iso)} at ${d.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' })}`
}

export function BackupPage() {
  useDocumentTitle('Backup & restore')
  const [backups, setBackups] = useState<BackupMeta[]>([])
  const [deviceBackups, setDeviceBackups] = useState<DeviceBackup[]>([])
  const [devices, setDevices] = useState<Device[]>([])
  const [pending, setPending] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [confirmId, setConfirmId] = useState<string | null>(null)

  async function load() {
    setError(null)
    try {
      const [snap, dev, list] = await Promise.all([
        api.listBackups().catch(() => ({ backups: [] as BackupMeta[] })),
        api.listDeviceBackups(),
        api.listDevices(),
      ])
      setBackups(snap.backups)
      setDeviceBackups(dev.backups)
      setPending(Object.fromEntries(dev.pending_requests.map((p) => [p.device_id, p.requested_at])))
      setDevices(list.devices)
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

  async function onRequest(d: Device) {
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      const r = await api.requestDeviceBackup(d.device_id)
      setPending((prev) => ({ ...prev, [d.device_id]: r.queued_at }))
      setMsg(`${d.device_name || 'Pocket'} will back up the next time it checks in.`)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    } finally {
      setBusy(false)
    }
  }

  async function onCreate() {
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      await api.createBackup()
      setMsg('Snapshot saved.')
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
      setMsg('Snapshot restored. Pocket updates when it next syncs.')
      setConfirmId(null)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't restore that snapshot.")
    } finally {
      setBusy(false)
    }
  }

  const latestDevice = deviceBackups[0]
  const latest = backups[0]

  return (
    <div className="page stack">
      <PageHeader
        title="Backup"
        back={{ to: '/account', label: 'Account' }}
        subtitle="Notes and Lists are always kept in Pocket Cloud. Backups add your Pocket's settings, alarms and reading places — a new Pocket offers to restore them during setup."
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}
      {msg ? <p className="badge badge--accent">{msg}</p> : null}

      {loading ? (
        <SkeletonList rows={3} />
      ) : (
        <>
          {latestDevice ? (
            <Group
              title="Latest Pocket backup"
              footer={`From ${latestDevice.device_name || 'Pocket'} · ${relativeTime(latestDevice.created_at)}`}
            >
              <Row icon="backup" tone="accent" title={backupWhen(latestDevice.created_at)} detail={latestDevice.device_name || 'Pocket'} />
              <Row icon="settings" title="Settings" value={latestDevice.settings_count ? 'Saved' : 'None'} />
              <Row icon="bell" title="Alarms" value={plural(latestDevice.alarm_count, 'alarm')} />
              <Row icon="reading" title="Reading places" value={plural(latestDevice.reading_count, 'book')} />
            </Group>
          ) : (
            <EmptyState icon="backup" title="No Pocket backup yet">
              Pocket backs itself up to Pocket Cloud. Ask for one below and it happens at its next check-in.
            </EmptyState>
          )}

          {devices.length ? (
            <Group
              title="Back up now"
              footer="Pocket backs up the next time it checks in — usually within a few minutes when it's on Wi‑Fi."
            >
              {devices.map((d) => {
                const waiting = pending[d.device_id]
                return (
                  <Row
                    key={d.id}
                    icon="device"
                    title={d.device_name || 'Pocket'}
                    detail={waiting ? 'Waiting for check-in' : d.last_seen_at ? `Seen ${relativeTime(d.last_seen_at)}` : 'Not checked in yet'}
                    value={waiting ? 'Requested' : 'Back up now'}
                    onClick={() => void onRequest(d)}
                    disabled={busy}
                    chevron={false}
                  />
                )
              })}
            </Group>
          ) : null}

          {deviceBackups.length > 1 ? (
            <Group title="Earlier Pocket backups">
              {deviceBackups.slice(1).map((b) => (
                <Row
                  key={b.id}
                  title={backupWhen(b.created_at)}
                  detail={`${b.device_name || 'Pocket'} · ${plural(b.alarm_count, 'alarm')} · ${plural(b.reading_count, 'book')}`}
                />
              ))}
            </Group>
          ) : null}

          <Group title="Notes & Lists snapshots" footer="A copy of every Note and List at one moment, if you ever want to roll back.">
            <Row icon="backup" title="Save a snapshot" onClick={() => void onCreate()} disabled={busy} chevron />
            {latest ? (
              <>
                <Row icon="download" title="Download latest" onClick={() => void onDownload()} disabled={busy} chevron />
                <Row icon="sync" title="Restore latest…" onClick={() => setConfirmId(latest.id)} disabled={busy} chevron />
              </>
            ) : null}
            {backups.map((b) => (
              <Row key={b.id} title={formatDate(b.created_at)} detail={`${b.note_count} notes · ${b.list_count} lists`} />
            ))}
          </Group>
        </>
      )}

      {confirmId ? (
        <div className="panel stack" role="alertdialog" aria-label="Restore snapshot">
          <p>Restore replaces Cloud Notes &amp; Lists with this snapshot. Pocket syncs after the restore.</p>
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
