import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api, isNetworkError, nowIso } from '../lib/api'
import { enqueueMutation } from '../lib/queue'
import type { PocketList } from '../lib/types'
import { relativeTime } from '../lib/utils'
import { ErrorState } from '../components/ErrorState'
import { useDocumentTitle } from '../components/useDocumentTitle'
import { Icon } from '../components/Icon'
import { EmptyState, PageHeader, Row, SkeletonList } from '../components/ui'

export function ListsListPage() {
  useDocumentTitle('Lists')
  const navigate = useNavigate()
  const [lists, setLists] = useState<PocketList[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  async function load() {
    setLoading(true)
    setError(null)
    try {
      const res = await api.listLists()
      setLists(res.lists.filter((l) => !l.deleted_at))
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

  async function onNew() {
    const payload = {
      title: 'Untitled list',
      items: [] as [],
      updated_at: nowIso(),
      updated_by_device_id: 'pwa',
    }
    try {
      const list = await api.createList(payload)
      navigate(`/lists/${list.id}`)
    } catch (err) {
      if (isNetworkError(err)) {
        await enqueueMutation({
          kind: 'list.create',
          path: '/v1/lists',
          method: 'POST',
          body: { ...payload, id: crypto.randomUUID() },
        })
        setError("You're offline or the server is unreachable.")
      } else {
        setError('Something went wrong.')
      }
    }
  }

  return (
    <div className="page stack">
      <PageHeader
        title="Lists"
        subtitle={lists.length ? 'Checklists shared with Pocket' : undefined}
        action={
          <button type="button" className="btn btn-primary btn-sm" onClick={() => void onNew()}>
            <Icon name="plus" size={16} stroke={2.4} /> New
          </button>
        }
      />

      {error ? <ErrorState message={error} onRetry={() => void load()} /> : null}

      {loading ? (
        <SkeletonList rows={3} />
      ) : lists.length === 0 && !error ? (
        <EmptyState
          icon="lists"
          title="No lists yet"
          action={
            <button type="button" className="btn btn-primary" onClick={() => void onNew()}>
              Start a list
            </button>
          }
        >
          Groceries, packing, to-dos — tick items off on Pocket or here.
        </EmptyState>
      ) : (
        <ul className="group__list">
          {lists.map((l) => {
            const done = l.items.filter((i) => i.checked).length
            return (
              <Row
                key={l.id}
                icon="lists"
                to={`/lists/${l.id}`}
                title={l.title || 'Untitled'}
                detail={l.items.length ? `${done} of ${l.items.length} done` : 'Empty'}
                value={relativeTime(l.updated_at)}
              />
            )
          })}
        </ul>
      )}
    </div>
  )
}
