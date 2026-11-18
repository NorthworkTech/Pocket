import { useEffect, useRef, useState } from 'react'
import { api, isNetworkError } from '../lib/api'
import type { Book, ReadingProgress } from '../lib/types'
import { relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { EmptyState, Group, PageHeader, Row, SkeletonList } from '../components/ui'

const MAX_UPLOAD_BYTES = 20 * 1024 * 1024

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

function guessFormat(name: string): 'epub' | 'txt' {
  return /\.epub$/i.test(name) ? 'epub' : 'txt'
}

function progressLabel(p: ReadingProgress): string {
  const parts = [`${p.percent}%`]
  if (p.pages > 0) parts.push(`page ${Math.min(Math.max(p.page, 1), p.pages)} of ${p.pages}`)
  const when = relativeTime(p.updated_at)
  if (when) parts.push(`read ${when}`)
  return parts.join(' · ')
}

/** Thin bar + "42% · page 120 of 286 · read 2h ago" under a book row. */
function BookProgress({ progress, title }: { progress: ReadingProgress; title: string }) {
  const pct = Math.max(0, Math.min(100, Math.round(progress.percent)))
  return (
    <span className="book-progress">
      <span
        className="progress"
        role="progressbar"
        aria-label={`${title} reading progress`}
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={pct}
      >
        <span className="progress__fill" style={{ width: `${pct}%` }} />
      </span>
      <span className="book-progress__label">{progressLabel(progress)}</span>
    </span>
  )
}

export function BooksPage() {
  useDocumentTitle('Reading')
  const inputRef = useRef<HTMLInputElement>(null)
  const [books, setBooks] = useState<Book[]>([])
  const [error, setError] = useState<string | null>(null)
  const [msg, setMsg] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [uploading, setUploading] = useState(false)

  async function load() {
    setLoading(true)
    setError(null)
    try {
      const res = await api.listBooks()
      setBooks(res.books)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't load your library.")
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
    if (file.size > MAX_UPLOAD_BYTES) {
      setError(`Keep each book under ${formatBytes(MAX_UPLOAD_BYTES)}.`)
      return
    }
    const format = guessFormat(file.name)
    if (!/\.(epub|txt)$/i.test(file.name)) {
      setError('Upload an EPUB or plain-text (.txt) file.')
      return
    }
    setUploading(true)
    try {
      const file_b64 = await fileToBase64(file)
      await api.uploadBook({
        title: file.name.replace(/\.(epub|txt)$/i, ''),
        filename: file.name,
        format,
        file_b64,
      })
      setMsg(`Uploaded ${file.name}. On Pocket: Home → Reading → Sync from Companion.`)
      await load()
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else if (err && typeof err === 'object' && 'message' in err) setError(String((err as { message: string }).message))
      else setError("Couldn't upload that book.")
    } finally {
      setUploading(false)
      if (inputRef.current) inputRef.current.value = ''
    }
  }

  async function onDelete(id: string, title: string) {
    if (!window.confirm(`Remove “${title}” from your Pocket Cloud library?`)) return
    setError(null)
    try {
      await api.deleteBook(id)
      setMsg('Removed from Cloud. Re-sync on Pocket to drop the local copy.')
      await load()
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't delete that book.")
    }
  }

  return (
    <div className="page stack">
      <PageHeader
        title="Reading"
        back={{ to: '/home', label: 'Home' }}
        action={
          <button type="button" className="btn btn-primary btn-sm" disabled={uploading} onClick={() => inputRef.current?.click()}>
            <Icon name="upload" size={16} stroke={2.2} /> {uploading ? 'Uploading…' : 'Upload'}
          </button>
        }
      />
      <input
        ref={inputRef}
        type="file"
        accept=".epub,.txt,application/epub+zip,text/plain"
        hidden
        onChange={(e) => void onPick(e.target.files?.[0] ?? null)}
      />

      <div className="panel row" style={{ padding: '0.875rem 1rem', alignItems: 'flex-start' }}>
        <span className="cell__icon">
          <Icon name="info" size={18} stroke={2} />
        </span>
        <p className="muted" style={{ fontSize: '0.875rem' }}>
          EPUB or plain text up to {formatBytes(MAX_UPLOAD_BYTES)}. Pocket sets books in a serif face, paginated for
          e-ink. Big libraries sync best with a microSD card.
        </p>
      </div>

      {msg ? <p className="badge badge--accent">{msg}</p> : null}
      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      {loading ? (
        <SkeletonList rows={3} />
      ) : books.length === 0 && !error ? (
        <EmptyState icon="reading" title="No books yet">
          Upload an EPUB or TXT, then Sync on Pocket to start reading.
        </EmptyState>
      ) : (
        <Group title={`${books.length} ${books.length === 1 ? 'book' : 'books'}`}>
          {books.map((b) => (
            <Row
              key={b.id}
              icon="reading"
              title={b.title || b.filename}
              detail={
                <>
                  <span className="book-meta">
                    {`${b.author ? `${b.author} · ` : ''}${(b.format || 'txt').toUpperCase()} · ${formatBytes(b.size_bytes || b.size || 0)}`}
                  </span>
                  {b.progress ? <BookProgress progress={b.progress} title={b.title || b.filename} /> : null}
                </>
              }
              value={
                <button type="button" className="btn btn-ghost btn-sm" onClick={() => void onDelete(b.id, b.title || b.filename)}>
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
