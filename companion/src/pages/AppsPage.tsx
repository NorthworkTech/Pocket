import { useEffect, useMemo, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import { ApiError, type AvailableApp, type InstalledApp } from '../lib/types'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Group, PageHeader, Row, SkeletonList } from '../components/ui'

function errorText(err: unknown): string {
  if (isNetworkError(err)) return "You're offline or the server is unreachable."
  if (err instanceof ApiError && err.message) return err.message
  return 'Something went wrong.'
}

/** 32×32 P4 icon → data URL (drawn on a canvas so it stays crisp). */
function iconUrl(b64: string): string | null {
  try {
    const bin = atob(b64)
    const m = /^P4\s+32\s+32\s/.exec(bin.slice(0, 16))
    if (!m) return null
    const off = m[0].length
    const c = document.createElement('canvas')
    c.width = 32
    c.height = 32
    const ctx = c.getContext('2d')
    if (!ctx) return null
    const img = ctx.createImageData(32, 32)
    for (let y = 0; y < 32; y++)
      for (let x = 0; x < 32; x++) {
        const on = (bin.charCodeAt(off + y * 4 + (x >> 3)) >> (7 - (x & 7))) & 1
        const i = (y * 32 + x) * 4
        const v = on ? 20 : 255
        img.data[i] = img.data[i + 1] = img.data[i + 2] = v
        img.data[i + 3] = 255
      }
    ctx.putImageData(img, 0, 0)
    return c.toDataURL()
  } catch {
    return null
  }
}

function AppIcon({ b64 }: { b64: string }) {
  const src = useMemo(() => iconUrl(b64), [b64])
  return src ? (
    <img src={src} alt="" width={32} height={32} style={{ imageRendering: 'pixelated', borderRadius: 6 }} />
  ) : null
}

export function AppsPage() {
  useDocumentTitle('Apps')
  const [installed, setInstalled] = useState<InstalledApp[]>([])
  const [available, setAvailable] = useState<AvailableApp[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const returnUrl = `${window.location.origin}${import.meta.env.BASE_URL.replace(/\/$/, '')}/apps`

  async function load() {
    setLoading(true)
    setError(null)
    try {
      const r = await api.listApps()
      setInstalled(r.installed)
      setAvailable(r.available)
    } catch (err) {
      setError(errorText(err))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    void load()
  }, [])

  const byId = new Map(available.map((a) => [a.id, a]))
  const notInstalled = available.filter((a) => !installed.some((i) => i.id === a.id))

  async function onInstall(app: AvailableApp) {
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      const r = await api.installApp(app.id, returnUrl)
      if (r.needs_auth && r.auth_url) {
        window.location.assign(r.auth_url)
        return
      }
      setMsg(r.needs_auth ? r.message || `${app.name} needs a sign-in that isn't available yet.` : `${app.name} is on its way to your Pocket.`)
      await load()
    } catch (err) {
      setError(errorText(err))
    } finally {
      setBusy(false)
    }
  }

  async function onConnect(app: InstalledApp) {
    setBusy(true)
    setError(null)
    try {
      const r = await api.appAuthStart(app.id, returnUrl)
      window.location.assign(r.auth_url)
    } catch (err) {
      setError(errorText(err))
      setBusy(false)
    }
  }

  async function onRemove(app: InstalledApp) {
    setBusy(true)
    setError(null)
    try {
      await api.uninstallApp(app.id)
      setMsg(`${app.name} removed from your Pocket.`)
      await load()
    } catch (err) {
      setError(errorText(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="page stack">
      <PageHeader
        title="Apps"
        back={{ to: '/account', label: 'Account' }}
        subtitle="Apps from Northwork appear on your Pocket's Home screen."
      />
      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}
      {msg ? <p className="badge badge--accent">{msg}</p> : null}

      {loading ? (
        <SkeletonList rows={2} />
      ) : (
        <>
          {installed.length ? (
            <Group title="On your Pocket" footer="Your Pocket picks up changes within a few minutes.">
              {installed.map((a) => (
                <Row
                  key={a.id}
                  leading={byId.get(a.id) ? <AppIcon b64={byId.get(a.id)!.icon_pbm_b64} /> : undefined}
                  title={a.name}
                  detail={a.auth_connected ? byId.get(a.id)?.developer.name : 'Needs you to sign in'}
                  value={
                    a.auth_connected ? (
                      <span className="cell__title--danger">Remove</span>
                    ) : (
                      <span>Sign in</span>
                    )
                  }
                  onClick={() => void (a.auth_connected ? onRemove(a) : onConnect(a))}
                  chevron={false}
                  disabled={busy}
                />
              ))}
            </Group>
          ) : null}
          {notInstalled.length ? (
            <Group title="Available">
              {notInstalled.map((a) => (
                <Row
                  key={a.id}
                  leading={<AppIcon b64={a.icon_pbm_b64} />}
                  title={a.name}
                  detail={a.summary || a.developer.name}
                  value={<span>Add</span>}
                  onClick={() => void onInstall(a)}
                  chevron={false}
                  disabled={busy}
                />
              ))}
            </Group>
          ) : null}
          {!installed.length && !notInstalled.length ? (
            <p className="muted">No apps yet. Developers publish Pocket apps on northwork.dev.</p>
          ) : null}
        </>
      )}
    </div>
  )
}
