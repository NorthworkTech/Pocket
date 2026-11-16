import { useDocumentTitle } from '../components/useDocumentTitle'
import { EmptyState, PageHeader } from '../components/ui'

/** Spec §6.6 — Coming soon placeholder (Passes never sync in Cloud v1). */
export function PassesPage() {
  useDocumentTitle('Passes')

  return (
    <div className="page stack">
      <PageHeader title="Passes" back={{ to: '/home', label: 'Home' }} />
      <EmptyState icon="pass" title="Coming soon">
        Boarding passes and tickets from your Pocket will show here in a free update.
      </EmptyState>
    </div>
  )
}
