import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api, isNetworkError, nowIso } from '../lib/api'
import { enqueueMutation } from '../lib/queue'
import type { Note } from '../lib/types'
import { relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { EmptyState, PageHeader, Row, SkeletonList } from '../components/ui'

export function NotesListPage() {
  useDocumentTitle('Notes')
  const navigate = useNavigate()
  const [notes, setNotes] = useState<Note[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [creating, setCreating] = useState(false)

  async function load() {
    setLoading(true)
    setError(null)
    try {
      const res = await api.listNotes()
      setNotes(res.notes.filter((n) => !n.deleted_at))
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError("Couldn't load notes.")
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    void load()
  }, [])

  async function onNew() {
    setCreating(true)
    const payload = {
      title: 'Untitled',
      body: '',
      updated_at: nowIso(),
      updated_by_device_id: 'pwa',
    }
    try {
      const note = await api.createNote(payload)
      navigate(`/notes/${note.id}`)
    } catch (err) {
      if (isNetworkError(err)) {
        const id = crypto.randomUUID()
        await enqueueMutation({
          kind: 'note.create',
          path: '/v1/notes',
          method: 'POST',
          body: { ...payload, id },
        })
        setError("You're offline or the server is unreachable.")
      } else {
        setError('Something went wrong.')
      }
    } finally {
      setCreating(false)
    }
  }

  return (
    <div className="page stack">
      <PageHeader
        title="Notes"
        subtitle={notes.length ? `${notes.length} ${notes.length === 1 ? 'note' : 'notes'} · synced with Pocket` : undefined}
        action={
          <button type="button" className="btn btn-primary btn-sm" disabled={creating} onClick={() => void onNew()}>
            <Icon name="plus" size={16} stroke={2.4} /> New
          </button>
        }
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      {loading ? (
        <SkeletonList rows={4} />
      ) : notes.length === 0 && !error ? (
        <EmptyState
          icon="notes"
          title="No notes yet"
          action={
            <button type="button" className="btn btn-primary" disabled={creating} onClick={() => void onNew()}>
              Write a note
            </button>
          }
        >
          Hold the side button on Pocket to dictate. Notes sync here automatically.
        </EmptyState>
      ) : (
        <ul className="group__list">
          {notes.map((n) => (
            <Row
              key={n.id}
              to={`/notes/${n.id}`}
              title={n.title || 'Untitled'}
              detail={n.body ? n.body.replace(/\s+/g, ' ').slice(0, 90) : 'No text yet'}
              value={relativeTime(n.updated_at)}
            />
          ))}
        </ul>
      )}
    </div>
  )
}
