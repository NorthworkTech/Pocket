import { useEffect, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import type { Connector, ConnectorProvider } from '../lib/types'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Group, PageHeader, Row } from '../components/ui'

const LABELS: Record<ConnectorProvider, string> = {
  drive: 'Google Drive',
  dropbox: 'Dropbox',
  onedrive: 'OneDrive',
}

const ORDER: ConnectorProvider[] = ['drive', 'dropbox', 'onedrive']

export function ConnectorsPage() {
  useDocumentTitle('Connectors')
  const [connectors, setConnectors] = useState<Connector[]>([])
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState<ConnectorProvider | null>(null)

  async function load() {
    setError(null)
    try {
      const res = await api.listConnectors()
      setConnectors(res.connectors)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    }
  }

  useEffect(() => {
    void load()
  }, [])

  function statusFor(provider: ConnectorProvider): Connector['status'] {
    return connectors.find((c) => c.provider === provider)?.status ?? 'disconnected'
  }

  async function onConnect(provider: ConnectorProvider) {
    setBusy(provider)
    setError(null)
    try {
      const { url } = await api.connectProvider(provider)
      window.location.href = url
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
      setBusy(null)
    }
  }

  async function onDisconnect(provider: ConnectorProvider) {
    const label = LABELS[provider]
    if (!window.confirm(`Disconnect ${label}? Copies already saved there stay put.`)) return
    setBusy(provider)
    setError(null)
    try {
      await api.disconnectProvider(provider)
      await load()
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    } finally {
      setBusy(null)
    }
  }

  return (
    <div className="page stack">
      <PageHeader
        title="Connectors"
        back={{ to: '/account', label: 'Account' }}
        subtitle="Keep a copy of your Notes and Lists where you already work."
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      <Group footer="Pocket exports copies — your originals stay in Pocket Cloud.">
        {ORDER.map((provider) => {
          const connected = statusFor(provider) === 'connected'
          return (
            <Row
              key={provider}
              icon="cloud"
              tone={connected ? 'accent' : 'default'}
              title={LABELS[provider]}
              detail={connected ? 'Connected' : 'Not connected'}
              value={
                connected ? (
                  <button
                    type="button"
                    className="btn btn-secondary btn-sm"
                    disabled={busy === provider}
                    onClick={() => void onDisconnect(provider)}
                  >
                    Disconnect
                  </button>
                ) : (
                  <button
                    type="button"
                    className="btn btn-primary btn-sm"
                    disabled={busy === provider}
                    onClick={() => void onConnect(provider)}
                  >
                    Connect
                  </button>
                )
              }
            />
          )
        })}
      </Group>
    </div>
  )
}
