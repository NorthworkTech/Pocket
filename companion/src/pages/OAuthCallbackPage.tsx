import { useEffect, useState } from 'react'
import { useParams } from 'react-router-dom'
import { api } from '../lib/api'
import { useDocumentTitle } from '../components/useDocumentTitle'

/**
 * "Log in with <provider>" for Pocket apps lands here (a stable Northwork URL the provider
 * registers). Pocket Cloud finishes the sign-in with the code and the one-time state, keeps
 * the tokens, and tells us where to go back to (the Apps page or the northwork.dev store).
 */
export function OAuthCallbackPage() {
  useDocumentTitle('Signing in')
  const { provider = '' } = useParams()
  const [message, setMessage] = useState('Finishing sign-in…')
  const [back, setBack] = useState<string | null>(null)

  useEffect(() => {
    const q = new URLSearchParams(window.location.search)
    const params = {
      code: q.get('code') || undefined,
      state: q.get('state') || undefined,
      error: q.get('error') || undefined,
    }
    // Keep the one-time code out of history and any later Referer.
    window.history.replaceState(null, '', window.location.pathname)
    let cancelled = false
    api
      .finishAppOAuth(provider, params)
      .then((r) => {
        if (cancelled) return
        if (r.ok) {
          window.location.replace(r.return_url)
        } else {
          setMessage(r.message || "Sign-in didn't finish. Try again.")
          setBack(r.return_url)
        }
      })
      .catch((err: unknown) => {
        if (cancelled) return
        const e = err as { message?: string; data?: { return_url?: string } }
        setMessage(e?.message || "Sign-in didn't finish. Try again.")
        setBack(e?.data?.return_url || `${import.meta.env.BASE_URL.replace(/\/$/, '')}/apps`)
      })
    return () => {
      cancelled = true
    }
  }, [provider])

  return (
    <div className="page stack" style={{ paddingTop: 48 }}>
      <p>{message}</p>
      {back ? (
        <a className="btn btn-primary" href={back}>
          Back
        </a>
      ) : null}
    </div>
  )
}
