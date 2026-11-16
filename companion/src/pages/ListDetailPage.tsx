import { type FormEvent, useEffect, useState } from 'react'
import { Link, useNavigate, useParams } from 'react-router-dom'
import { api, isNetworkError, nowIso } from '../lib/api'
import { enqueueMutation } from '../lib/queue'
import type { ListItem } from '../lib/types'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { Group, Row } from '../components/ui'

export function ListDetailPage() {
  const { id = '' } = useParams()
  const navigate = useNavigate()
  const [title, setTitle] = useState('')
  const [items, setItems] = useState<ListItem[]>([])
  const [newItem, setNewItem] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [confirmDelete, setConfirmDelete] = useState(false)

  useDocumentTitle(title || 'List')

  async function load() {
    setError(null)
    try {
      const list = await api.getList(id)
      setTitle(list.title)
      setItems(list.items)
    } catch (err) {
      if (isNetworkError(err)) setError("You're offline or the server is unreachable.")
      else setError('Something went wrong.')
    }
  }

  useEffect(() => {
    void load()
  }, [id])

  async function persist(nextTitle: string, nextItems: ListItem[]) {
    const payload = {
      title: nextTitle,
      items: nextItems,
      updated_at: nowIso(),
      updated_by_device_id: 'pwa',
    }
    try {
      const list = await api.updateList(id, payload)
      setTitle(list.title)
      setItems(list.items)
    } catch (err) {
      if (isNetworkError(err)) {
        await enqueueMutation({
          kind: 'list.update',
          path: `/v1/lists/${id}`,
          method: 'PATCH',
          body: payload,
        })
        setError("Couldn't sync. Will retry.")
      } else {
        setError('Something went wrong.')
      }
    }
  }

  function toggle(itemId: string) {
    const next = items.map((it) =>
      it.id === itemId ? { ...it, checked: !it.checked, updated_at: nowIso() } : it,
    )
    setItems(next)
    void persist(title, next)
  }

  function onAdd(e: FormEvent) {
    e.preventDefault()
    const text = newItem.trim()
    if (!text) return
    const item: ListItem = {
      id: crypto.randomUUID(),
      text,
      checked: false,
      updated_at: nowIso(),
    }
    const next = [...items, item]
    setItems(next)
    setNewItem('')
    void persist(title, next)
  }

  function onSaveTitle(e: FormEvent) {
    e.preventDefault()
    void persist(title, items)
  }

  async function onDelete() {
    const payload = { updated_at: nowIso(), updated_by_device_id: 'pwa' }
    try {
      await api.deleteList(id, payload)
      navigate('/lists', { replace: true })
    } catch (err) {
      if (isNetworkError(err)) {
        await enqueueMutation({
          kind: 'list.delete',
          path: `/v1/lists/${id}`,
          method: 'DELETE',
          body: payload,
        })
        setError("Couldn't sync. Will retry.")
      } else {
        setError('Something went wrong.')
      }
    }
  }

  const done = items.filter((i) => i.checked).length

  return (
    <div className="page stack">
      <Link to="/lists" className="back-link">
        <Icon name="chevron-left" size={20} stroke={2.2} />
        Lists
      </Link>

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      <form className="editor" onSubmit={onSaveTitle}>
        <label htmlFor="list-title" className="sr-only">
          Title
        </label>
        <input id="list-title" value={title} onChange={(e) => setTitle(e.target.value)} placeholder="List name" />
      </form>

      {items.length ? (
        <p className="eyebrow" style={{ padding: '0 1rem' }}>
          {done} of {items.length} done
        </p>
      ) : null}

      <div className="list">
        {items.map((it) => (
          <label key={it.id} className="check-row">
            <input type="checkbox" checked={it.checked} onChange={() => toggle(it.id)} />
            <span>{it.text}</span>
          </label>
        ))}
        <form className="check-row" onSubmit={onAdd}>
          <Icon name="plus" size={20} stroke={2.2} className="cell__chevron" />
          <input
            className="input"
            style={{ flex: 1, border: 0, padding: 0, minHeight: '2rem', boxShadow: 'none' }}
            value={newItem}
            onChange={(e) => setNewItem(e.target.value)}
            placeholder="Add an item"
            aria-label="Add item"
          />
          {newItem.trim() ? (
            <button type="submit" className="btn btn-primary btn-sm">
              Add
            </button>
          ) : null}
        </form>
      </div>

      <Group>
        <Row icon="trash" tone="danger" title="Delete list" onClick={() => setConfirmDelete(true)} />
      </Group>

      {confirmDelete ? (
        <div className="panel stack" role="alertdialog" aria-label="Delete list">
          <p>Delete this list on Pocket and in Pocket Cloud? This can't be undone.</p>
          <div className="actions-row">
            <button type="button" className="btn btn-danger" onClick={() => void onDelete()}>
              Delete list
            </button>
            <button type="button" className="btn btn-secondary" onClick={() => setConfirmDelete(false)}>
              Cancel
            </button>
          </div>
        </div>
      ) : null}
    </div>
  )
}
