import type { ReactNode } from 'react'
import { Link } from 'react-router-dom'
import { Icon, type IconName } from './Icon'

/** Large-title page header with optional back link and trailing action. */
export function PageHeader({
  title,
  subtitle,
  back,
  action,
}: {
  title: ReactNode
  subtitle?: ReactNode
  back?: { to: string; label: string }
  action?: ReactNode
}) {
  return (
    <header className="page-header">
      {back ? (
        <Link to={back.to} className="back-link">
          <Icon name="chevron-left" size={20} stroke={2.2} />
          {back.label}
        </Link>
      ) : null}
      <div className="page-header__row">
        <h1 className="page-title">{title}</h1>
        {action ? <div className="page-header__action">{action}</div> : null}
      </div>
      {subtitle ? <p className="page-subtitle">{subtitle}</p> : null}
    </header>
  )
}

/** Centered empty state with an icon medallion. */
export function EmptyState({
  icon,
  title,
  children,
  action,
}: {
  icon: IconName
  title: string
  children?: ReactNode
  action?: ReactNode
}) {
  return (
    <div className="empty-state">
      <div className="empty-state__icon">
        <Icon name={icon} size={28} />
      </div>
      <h2>{title}</h2>
      {children ? <p className="muted">{children}</p> : null}
      {action ? <div className="empty-state__action">{action}</div> : null}
    </div>
  )
}

/** Inset grouped list section (iOS-settings style) with optional header/footer. */
export function Group({ title, footer, children }: { title?: string; footer?: ReactNode; children: ReactNode }) {
  return (
    <section className="group">
      {title ? <h2 className="group__title">{title}</h2> : null}
      <ul className="group__list">{children}</ul>
      {footer ? <p className="group__footer">{footer}</p> : null}
    </section>
  )
}

type RowProps = {
  icon?: IconName
  /** Custom leading visual (e.g. an app's own icon) in place of `icon`. */
  leading?: ReactNode
  tone?: 'default' | 'accent' | 'danger'
  title: ReactNode
  detail?: ReactNode
  value?: ReactNode
  to?: string
  href?: string
  onClick?: () => void
  chevron?: boolean
  disabled?: boolean
}

/** One row in a Group. Renders as a Link, external anchor, button, or static row. */
export function Row({ icon, leading, tone = 'default', title, detail, value, to, href, onClick, chevron, disabled }: RowProps) {
  const body = (
    <>
      {leading ? (
        <span className={`cell__icon cell__icon--${tone}`}>{leading}</span>
      ) : icon ? (
        <span className={`cell__icon cell__icon--${tone}`}>
          <Icon name={icon} size={18} stroke={2} />
        </span>
      ) : null}
      <span className="cell__text">
        <span className={`cell__title${tone === 'danger' ? ' cell__title--danger' : ''}`}>{title}</span>
        {detail ? <span className="cell__detail">{detail}</span> : null}
      </span>
      {value !== undefined && value !== null ? <span className="cell__value">{value}</span> : null}
      {chevron ?? !!(to || href) ? <Icon name={href ? 'external' : 'chevron-right'} size={16} stroke={2.2} className="cell__chevron" /> : null}
    </>
  )
  return (
    <li>
      {to ? (
        <Link className="cell" to={to}>
          {body}
        </Link>
      ) : href ? (
        <a className="cell" href={href} target="_blank" rel="noreferrer">
          {body}
        </a>
      ) : onClick ? (
        <button type="button" className="cell" onClick={onClick} disabled={disabled}>
          {body}
        </button>
      ) : (
        <div className="cell cell--static">{body}</div>
      )}
    </li>
  )
}

/** Small status pill. */
export function Badge({ tone = 'neutral', children }: { tone?: 'neutral' | 'accent' | 'warn' | 'danger'; children: ReactNode }) {
  return <span className={`badge badge--${tone}`}>{children}</span>
}

/** Loading placeholder rows. */
export function SkeletonList({ rows = 3 }: { rows?: number }) {
  return (
    <ul className="group__list" aria-busy="true" aria-label="Loading">
      {Array.from({ length: rows }, (_, i) => (
        <li key={i}>
          <div className="cell cell--static">
            <span className="skeleton skeleton--icon" />
            <span className="cell__text">
              <span className="skeleton skeleton--line" />
              <span className="skeleton skeleton--line skeleton--short" />
            </span>
          </div>
        </li>
      ))}
    </ul>
  )
}
