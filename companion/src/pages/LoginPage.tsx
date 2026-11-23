import { type FormEvent, useEffect, useRef, useState } from 'react'
import { Link, Navigate, useSearchParams } from 'react-router-dom'
import { api, getSessionToken, isApiConfigured, isNetworkError } from '../lib/api'
import { useAuth } from '../lib/auth'
import { ApiError } from '../lib/types'
import { ErrorState } from '../components/ErrorState'
import { WordMark } from '../components/WordMark'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { isCompanionAppPath, isPhoneCompanionClient } from '../lib/utils'

type Mode = 'signin' | 'signup'
type State = 'idle' | 'busy' | 'error'

export function LoginPage() {
  useDocumentTitle('Sign in')
  const { isAuthenticated, loading, offline, error: authError, refresh } = useAuth()
  const [params] = useSearchParams()
  const returnTo = params.get('return_to') || '/home'
  const modeParam = params.get('mode')
  const [mode, setMode] = useState<Mode>(modeParam === 'signup' ? 'signup' : 'signin')
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [state, setState] = useState<State>('idle')
  const [error, setError] = useState<string | null>(null)

  const cloudReady = isApiConfigured()
  const phone = isPhoneCompanionClient()
  const safeReturn =
    !phone && isCompanionAppPath(returnTo.split('?')[0] || returnTo)
      ? `/get-companion?from=${encodeURIComponent(returnTo)}`
      : returnTo

  // On northwork.app, Pocket uses your Northwork account (one sign-in for everything).
  const onNorthwork =
    typeof window !== 'undefined' &&
    (window.location.hostname === 'northwork.app' || params.get('northwork') === '1')
  const triedSilent = useRef(false)
  const [useEmail, setUseEmail] = useState(false)

  async function continueWithNorthwork(silent: boolean) {
    setState('busy')
    setError(null)
    try {
      const r = await api.loginWithNorthwork()
      if (r === 'signin') {
        setState('idle')
        if (!silent) {
          const back = `${window.location.pathname}${window.location.search}`
          window.location.href = `/api/auth/login?next=${encodeURIComponent(back)}`
        }
        return
      }
      await refresh()
      setState('idle')
    } catch (err) {
      setState('error')
      setError(err instanceof ApiError ? err.message : "Couldn't sign in with Northwork.")
    }
  }

  useEffect(() => {
    if (onNorthwork && !loading && !isAuthenticated && cloudReady && !triedSilent.current) {
      triedSilent.current = true
      void continueWithNorthwork(true)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [onNorthwork, loading, isAuthenticated, cloudReady])

  if (!loading && isAuthenticated) {
    return <Navigate to={safeReturn} replace />
  }

  if (onNorthwork && !useEmail) {
    return (
      <div className="page stack auth-page">
        <WordMark to="/" />
        <div className="stack-sm">
          <h1>Sign in to Pocket</h1>
          <p className="muted">
            Pocket uses your Northwork account — the same one you use on northwork.app and in the Community.
          </p>
        </div>
        <div className="panel stack">
          <button
            type="button"
            className="btn btn-primary btn-block"
            disabled={state === 'busy'}
            onClick={() => void continueWithNorthwork(false)}
          >
            {state === 'busy' ? 'Signing in…' : 'Continue with Northwork'}
          </button>
          {error ? (
            <p role="alert" className="muted">
              {error}
            </p>
          ) : null}
          <p className="muted small">
            New to Northwork? You can create an account on the next screen. Already had a Pocket Cloud
            account? Use the same email and everything carries over.
          </p>
          <button type="button" className="btn btn-ghost btn-block" onClick={() => setUseEmail(true)}>
            Use email and password instead
          </button>
        </div>
      </div>
    )
  }

  async function onSubmit(e: FormEvent) {
    e.preventDefault()
    if (!cloudReady) {
      setError('Pocket Cloud is temporarily unavailable. Try again later.')
      return
    }
    setState('busy')
    setError(null)
    try {
      if (mode === 'signup') {
        await api.register(email.trim(), password)
      }
      await api.login(email.trim(), password)
      if (!getSessionToken()) {
        throw new ApiError("Couldn't save your sign-in on this device. Try again.", 0)
      }
      await refresh()
      setState('idle')
    } catch (err) {
      setState('error')
      if (isNetworkError(err)) {
        setError(
          err instanceof ApiError && err.code === 'not_connected'
            ? 'Pocket Cloud is temporarily unavailable. Try again later.'
            : "You're offline or the server is unreachable.",
        )
      } else if (err instanceof ApiError) {
        setError(err.message || (mode === 'signup' ? "Couldn't create account." : "Couldn't sign in."))
      } else {
        setError(mode === 'signup' ? "Couldn't create account." : "Couldn't sign in.")
      }
    }
  }

  return (
    <div className="page stack auth-page">
      <WordMark to="/" />
      <div className="stack-sm">
        <h1>{mode === 'signup' ? 'Create your account' : 'Sign in to Pocket'}</h1>
        <p className="muted">
          {mode === 'signup'
            ? 'Create a Pocket Cloud account with your email and a password (8+ characters). Companion runs on your phone.'
            : 'Use the email and password for your Pocket Cloud account.'}
        </p>
        {!phone ? (
          <p className="muted">
            Companion is phone-only — not a desktop website. On a Mac you can{' '}
            <Link to="/control">control Pocket</Link> (Wi‑Fi &amp; settings) or open Cloud Admin;{' '}
            <Link to="/get-companion">download Companion</Link> on your phone for pairing and everyday
            use.
          </p>
        ) : null}
      </div>

      {onNorthwork ? null : (
      <div className="panel" role="note">
        <p>
          <strong>Pocket has moved to Northwork.</strong> Companion now lives at{' '}
          <a href="https://northwork.app/pocket/app/">northwork.app/pocket/app</a> and uses your Northwork
          account. This address will stop working soon.
        </p>
      </div>
      )}

      {!cloudReady || offline ? (
        <div className="panel" role="status">
          <p className="muted">
            Pocket Cloud is temporarily unavailable. You can still open this page; try signing in again
            shortly.
          </p>
          {authError ? <p className="muted">{authError}</p> : null}
        </div>
      ) : null}

      <div className="segmented" role="tablist" aria-label="Account">
        <button
          type="button"
          role="tab"
          aria-selected={mode === 'signin'}
          className={mode === 'signin' ? 'segmented__item active' : 'segmented__item'}
          onClick={() => {
            setMode('signin')
            setState('idle')
            setError(null)
          }}
        >
          Sign in
        </button>
        <button
          type="button"
          role="tab"
          aria-selected={mode === 'signup'}
          className={mode === 'signup' ? 'segmented__item active' : 'segmented__item'}
          onClick={() => {
            setMode('signup')
            setState('idle')
            setError(null)
          }}
        >
          Create account
        </button>
      </div>

      <form className="panel stack" onSubmit={(e) => void onSubmit(e)}>
        <div className="field">
          <label htmlFor="email">Email</label>
          <input
            id="email"
            name="email"
            type="email"
            autoComplete="email"
            required
            value={email}
            onChange={(e) => setEmail(e.target.value)}
            disabled={state === 'busy' || !cloudReady}
          />
        </div>
        <div className="field">
          <label htmlFor="password">Password</label>
          <input
            id="password"
            name="password"
            type="password"
            autoComplete={mode === 'signup' ? 'new-password' : 'current-password'}
            required
            minLength={8}
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            disabled={state === 'busy' || !cloudReady}
          />
          {mode === 'signup' ? <p className="muted">At least 8 characters.</p> : null}
        </div>
        {error ? (
          error.includes('unreachable') || error.includes('unavailable') ? (
            <ErrorState message={error} onRetry={() => setError(null)} />
          ) : (
            <p className="error-box" role="alert">
              {error}
            </p>
          )
        ) : null}
        <button
          type="submit"
          className="btn btn-primary btn-block"
          disabled={state === 'busy' || !cloudReady}
        >
          {state === 'busy'
            ? mode === 'signup'
              ? 'Creating…'
              : 'Signing in…'
            : mode === 'signup'
              ? 'Create account'
              : 'Sign in'}
        </button>
      </form>

      <p className="footer-note">
        Pocket Cloud syncs Notes, Lists, Music, and Reading from your Pocket — managed in Companion on
        your phone.
      </p>
      {returnTo.startsWith('/pair') || returnTo.startsWith('/link') ? (
        <p className="muted" style={{ textAlign: 'center', fontSize: '0.875rem' }}>
          After sign-in you&apos;ll continue linking your Pocket.{' '}
          <Link to={returnTo}>Back to pair</Link>
        </p>
      ) : null}
    </div>
  )
}
