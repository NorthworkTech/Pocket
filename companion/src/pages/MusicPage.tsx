import { useEffect, useRef, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import type { MusicTrack } from '../lib/types'
import { relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { EmptyState, Group, PageHeader, Row, SkeletonList } from '../components/ui'

const FALLBACK_INTERNAL = 2 * 1024 * 1024
const FALLBACK_SD = 80 * 1024 * 1024

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
  return `${(n / (1024 * 1024)).toFixed(2)} MB`
}

async function fileToBase64(file: File): Promise<string> {
  const buf = await file.arrayBuffer()
  const bytes = new Uint8Array(buf)
  let binary = ''
  const chunk = 0x8000
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk))
  }
  return btoa(binary)
}

export function MusicPage() {
  useDocumentTitle('Music')
  const inputRef = useRef<HTMLInputElement>(null)
  const [tracks, setTracks] = useState<MusicTrack[]>([])
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [uploading, setUploading] = useState(false)
  const [maxBytes, setMaxBytes] = useState(FALLBACK_INTERNAL)
  const [sdPresent, setSdPresent] = useState(false)

  async function loadLimits() {
    try {
      const lim = await api.musicLimits()
      setSdPresent(Boolean(lim.sd_present))
      setMaxBytes(lim.max_upload_bytes || (lim.sd_present ? FALLBACK_SD : FALLBACK_INTERNAL))
    } catch {
      setMaxBytes(FALLBACK_INTERNAL)
      setSdPresent(false)
    }
  }

  async function load() {
    setLoading(true)
    setError(null)
    try {
      await loadLimits()
      const res = await api.listMusic()
      setTracks(res.tracks)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't load music.")
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    void load()
  }, [])

  async function onPick(file: File | null) {
    if (!file) return
    setMsg(null)
    setError(null)
    if (file.size > maxBytes) {
      setError(
        sdPresent
          ? `Keep each WAV under ${formatBytes(maxBytes)}.`
          : `Keep each WAV under ${formatBytes(FALLBACK_INTERNAL)} without an SD card, or insert a microSD in Pocket and Sync once to unlock up to ${formatBytes(FALLBACK_SD)}.`,
      )
      return
    }
    const name = file.name || 'track.wav'
    if (!/\.wav$/i.test(name) && file.type && !file.type.includes('wav')) {
      setError('Only WAV files play on Pocket right now.')
      return
    }
    setUploading(true)
    try {
      const audio_b64 = await fileToBase64(file)
      await api.uploadMusic({
        title: name.replace(/\.wav$/i, ''),
        filename: name.endsWith('.wav') || name.endsWith('.WAV') ? name : `${name}.wav`,
        mime: file.type || 'audio/wav',
        audio_b64,
      })
      setMsg(`Uploaded ${name}. On Pocket: Home → Music → Sync from Companion.`)
      await load()
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else if (err && typeof err === 'object' && 'message' in err) setError(String((err as { message: string }).message))
      else setError("Couldn't upload that track.")
    } finally {
      setUploading(false)
      if (inputRef.current) inputRef.current.value = ''
    }
  }

  async function onDelete(id: string, title: string) {
    if (!window.confirm(`Remove “${title}” from your Pocket Cloud library?`)) return
    setError(null)
    try {
      await api.deleteMusic(id)
      setMsg('Removed from Cloud. Re-sync on Pocket to drop the local copy.')
      await load()
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't delete that track.")
    }
  }

  return (
    <div className="page stack">
      <PageHeader
        title="Music"
        back={{ to: '/home', label: 'Home' }}
        action={
          <button type="button" className="btn btn-primary btn-sm" disabled={uploading} onClick={() => inputRef.current?.click()}>
            <Icon name="upload" size={16} stroke={2.2} /> {uploading ? 'Uploading…' : 'Upload'}
          </button>
        }
      />
      <input ref={inputRef} type="file" accept="audio/wav,.wav" hidden onChange={(e) => void onPick(e.target.files?.[0] ?? null)} />

      <div className="panel row" style={{ padding: '0.875rem 1rem', alignItems: 'flex-start' }}>
        <span className="cell__icon">
          <Icon name="info" size={18} stroke={2} />
        </span>
        <p className="muted" style={{ fontSize: '0.875rem' }}>
          {sdPresent
            ? `microSD detected — WAV uploads up to ${formatBytes(maxBytes)}. Sync on Pocket saves them to the card.`
            : `WAV files under ${formatBytes(FALLBACK_INTERNAL)} fit internal storage. Add a microSD card and Sync once for up to ${formatBytes(FALLBACK_SD)}.`}
        </p>
      </div>

      {msg ? <p className="badge badge--accent">{msg}</p> : null}
      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      {loading ? (
        <SkeletonList rows={3} />
      ) : tracks.length === 0 && !error ? (
        <EmptyState icon="music" title="No tracks yet">
          Upload a WAV, then Sync on Pocket to play it through the speaker.
        </EmptyState>
      ) : (
        <Group title={`${tracks.length} ${tracks.length === 1 ? 'track' : 'tracks'}`}>
          {tracks.map((t) => (
            <Row
              key={t.id}
              icon="music"
              title={t.title || t.filename}
              detail={`${formatBytes(t.size_bytes || t.size || 0)}${t.created_at ? ` · ${relativeTime(t.created_at)}` : ''}`}
              value={
                <button type="button" className="btn btn-ghost btn-sm" onClick={() => void onDelete(t.id, t.title || t.filename)}>
                  Remove
                </button>
              }
            />
          ))}
        </Group>
      )}
    </div>
  )
}
