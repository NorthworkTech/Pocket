import { Link } from 'react-router-dom'

/** Pocket mark (folded-corner square, as on the device lock screen) + wordmark. */
export function PocketMark({ size = 22 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" aria-hidden="true" className="pocket-mark">
      <rect x="2" y="2" width="20" height="20" rx="5" fill="none" stroke="currentColor" strokeWidth="2.6" />
      <path d="M13 2h4a5 5 0 0 1 5 5v4Z" fill="currentColor" />
    </svg>
  )
}

export function WordMark({ to = '/home' }: { to?: string }) {
  return (
    <Link to={to} className="wordmark">
      <PocketMark />
      <span>Pocket</span>
    </Link>
  )
}
