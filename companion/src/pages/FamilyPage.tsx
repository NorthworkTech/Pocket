import { type FormEvent, useEffect, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import { ApiError, type Family, type FamilyMember } from '../lib/types'
import { formatDate } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { Badge, Group, PageHeader, Row, SkeletonList } from '../components/ui'

function errorText(err: unknown): string {
  if (isNetworkError(err)) return "You're offline or the server is unreachable."
  if (err instanceof ApiError && err.message) return err.message
  return 'Something went wrong.'
}

function formatFamilyCode(code: string): string {
  return code.length === 8 ? `${code.slice(0, 4)}-${code.slice(4)}` : code
}

export function FamilyPage() {
  useDocumentTitle('Pocket Family')
  const [family, setFamily] = useState<Family | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [name, setName] = useState('')
  const [code, setCode] = useState('')
  const [confirmRemove, setConfirmRemove] = useState<FamilyMember | null>(null)
  const [confirmLeave, setConfirmLeave] = useState(false)

  async function load() {
    setLoading(true)
    setError(null)
    try {
      setFamily((await api.getFamily()).family)
    } catch (err) {
      setError(errorText(err))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    void load()
  }, [])

  async function run(action: () => Promise<Family | null>, done?: string) {
    setBusy(true)
    setError(null)
    setMsg(null)
    try {
      setFamily(await action())
      if (done) setMsg(done)
    } catch (err) {
      setError(errorText(err))
    } finally {
      setBusy(false)
    }
  }

  function onCreate(e: FormEvent) {
    e.preventDefault()
    if (!name.trim()) return
    void run(async () => (await api.createFamily(name.trim())).family, 'Family created. Share the code to invite people.')
  }

  function onJoin(e: FormEvent) {
    e.preventDefault()
    if (!code.trim()) return
    void run(async () => (await api.joinFamily(code.trim())).family, "You're in. Lists shared with the family now show up.")
  }

  async function onShare() {
    if (!family?.join_code) return
    const pretty = formatFamilyCode(family.join_code)
    const text = `Join "${family.name}" on Pocket: open the Pocket Companion app, go to Account → Pocket Family and enter ${pretty}.`
    try {
      if (navigator.share) {
        await navigator.share({ title: 'Join our Pocket Family', text })
        return
      }
    } catch (err) {
      if (err instanceof DOMException && err.name === 'AbortError') return
    }
    try {
      await navigator.clipboard.writeText(text)
      setMsg('Invite copied. Paste it in a message.')
    } catch {
      setMsg(`Your code is ${pretty}.`)
    }
  }

  const isOwner = family?.role === 'owner'

  return (
    <div className="page stack">
      <PageHeader
        title="Pocket Family"
        back={{ to: '/account', label: 'Account' }}
        subtitle={`Share lists with up to ${family?.max_members ?? 6} people. Everyone can tick things off here or on their Pocket.`}
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}
      {msg ? <p className="badge badge--accent">{msg}</p> : null}

      {loading ? (
        <SkeletonList rows={2} />
      ) : !family ? (
        <>
          <form className="panel stack" onSubmit={onCreate}>
            <h2 style={{ margin: 0 }}>Start a family</h2>
            <div className="field">
              <label htmlFor="family-name">Family name</label>
              <input
                id="family-name"
                value={name}
                maxLength={40}
                onChange={(e) => setName(e.target.value)}
                placeholder="The Riveras"
              />
            </div>
            <button type="submit" className="btn btn-primary" disabled={busy || !name.trim()}>
              Create family
            </button>
          </form>

          <form className="panel stack" onSubmit={onJoin}>
            <h2 style={{ margin: 0 }}>Join a family</h2>
            <p className="muted" style={{ margin: 0 }}>
              Got a code from someone? Enter it here.
            </p>
            <div className="field">
              <label htmlFor="family-code">Invite code</label>
              <input
                id="family-code"
                value={code}
                maxLength={9}
                autoCapitalize="characters"
                autoComplete="off"
                spellCheck={false}
                onChange={(e) => setCode(e.target.value.toUpperCase())}
                placeholder="ABCD-2345"
                style={{ letterSpacing: '0.12em', fontWeight: 600 }}
              />
            </div>
            <button type="submit" className="btn btn-secondary" disabled={busy || code.replace(/[^A-Z0-9]/gi, '').length < 8}>
              Join family
            </button>
          </form>
        </>
      ) : (
        <>
          <div className="panel stack-sm">
            <h2 style={{ margin: 0 }}>{family.name}</h2>
            <span className="muted">
              {family.members.length} of {family.max_members} people · {isOwner ? 'You run this family' : 'Member'}
            </span>
          </div>

          {isOwner ? (
            <div className="panel stack">
              <p className="eyebrow" style={{ margin: 0 }}>
                Invite code
              </p>
              {family.join_code ? (
                <>
                  <span className="family-code" aria-label={`Invite code ${family.join_code.split('').join(' ')}`}>
                    {formatFamilyCode(family.join_code)}
                  </span>
                  <p className="muted" style={{ margin: 0 }}>
                    Works until {formatDate(family.join_code_expires_at)}. They enter it under Account → Pocket Family.
                  </p>
                  <div className="actions-row">
                    <button type="button" className="btn btn-primary" onClick={() => void onShare()}>
                      <Icon name="share" size={16} stroke={2.2} /> Share invite
                    </button>
                    <button
                      type="button"
                      className="btn btn-secondary"
                      disabled={busy}
                      onClick={() => void run(async () => (await api.newFamilyCode()).family, 'New code ready. The old one no longer works.')}
                    >
                      New code
                    </button>
                  </div>
                </>
              ) : (
                <button
                  type="button"
                  className="btn btn-primary"
                  disabled={busy}
                  onClick={() => void run(async () => (await api.newFamilyCode()).family)}
                >
                  Make an invite code
                </button>
              )}
            </div>
          ) : null}

          <Group title="People" footer="Open a list and turn on “Share with family” to let everyone here see and edit it.">
            {family.members.map((m) => (
              <Row
                key={m.user_id}
                icon="user"
                title={m.is_me ? `${m.name} (you)` : m.name}
                detail={m.role === 'owner' ? 'Owner' : `Joined ${formatDate(m.joined_at)}`}
                value={
                  isOwner && !m.is_me ? (
                    <span className="cell__title--danger">Remove</span>
                  ) : m.role === 'owner' ? (
                    <Badge tone="accent">Owner</Badge>
                  ) : undefined
                }
                onClick={isOwner && !m.is_me ? () => setConfirmRemove(m) : undefined}
                chevron={false}
                disabled={busy}
              />
            ))}
          </Group>

          {confirmRemove ? (
            <div className="panel stack" role="alertdialog" aria-label="Remove from family">
              <p style={{ margin: 0 }}>
                Remove {confirmRemove.name}? They lose access to shared lists, and lists they shared stop being shared.
              </p>
              <div className="actions-row">
                <button
                  type="button"
                  className="btn btn-danger"
                  disabled={busy}
                  onClick={() =>
                    void run(async () => {
                      const r = await api.removeFamilyMember(confirmRemove.user_id)
                      setConfirmRemove(null)
                      return r.family
                    }, `${confirmRemove.name} was removed.`)
                  }
                >
                  Remove
                </button>
                <button type="button" className="btn btn-secondary" onClick={() => setConfirmRemove(null)}>
                  Cancel
                </button>
              </div>
            </div>
          ) : null}

          <Group>
            <Row
              icon={isOwner ? 'trash' : 'logout'}
              tone="danger"
              title={isOwner ? 'Delete family' : 'Leave family'}
              onClick={() => setConfirmLeave(true)}
            />
          </Group>

          {confirmLeave ? (
            <div className="panel stack" role="alertdialog" aria-label={isOwner ? 'Delete family' : 'Leave family'}>
              <p style={{ margin: 0 }}>
                {isOwner
                  ? 'Delete this family? Everyone is removed and shared lists go back to their owners.'
                  : 'Leave this family? Lists others shared stop showing up, and yours stop being shared.'}
              </p>
              <div className="actions-row">
                <button
                  type="button"
                  className="btn btn-danger"
                  disabled={busy}
                  onClick={() =>
                    void run(async () => {
                      if (isOwner) await api.deleteFamily()
                      else await api.leaveFamily()
                      setConfirmLeave(false)
                      return null
                    }, isOwner ? 'Family deleted.' : 'You left the family.')
                  }
                >
                  {isOwner ? 'Delete family' : 'Leave family'}
                </button>
                <button type="button" className="btn btn-secondary" onClick={() => setConfirmLeave(false)}>
                  Cancel
                </button>
              </div>
            </div>
          ) : null}
        </>
      )}
    </div>
  )
}
