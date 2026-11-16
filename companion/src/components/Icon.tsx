import type { ReactElement } from 'react'

/**
 * Companion icon set — drawn on the same 24-unit grid and stroke language as the
 * Pocket device icons (firmware ui.cpp), so phone and device read as one product.
 */
export type IconName =
  | 'home'
  | 'notes'
  | 'lists'
  | 'music'
  | 'reading'
  | 'device'
  | 'settings'
  | 'cloud'
  | 'backup'
  | 'link'
  | 'pass'
  | 'plus'
  | 'chevron-right'
  | 'chevron-left'
  | 'wifi'
  | 'battery'
  | 'trash'
  | 'share'
  | 'sync'
  | 'user'
  | 'card'
  | 'lock'
  | 'mic'
  | 'check'
  | 'upload'
  | 'download'
  | 'external'
  | 'info'
  | 'qr'
  | 'phone'
  | 'code'
  | 'logout'
  | 'sparkle'

const paths: Record<IconName, ReactElement> = {
  home: <path d="M3.5 11.5 12 4l8.5 7.5M6 9.5V20h12V9.5" />,
  notes: (
    <>
      <rect x="5" y="2.5" width="14" height="19" rx="2.5" />
      <path d="M8.5 8h7M8.5 12h7M8.5 16h4" />
    </>
  ),
  lists: (
    <>
      <rect x="3.5" y="4" width="5" height="5" rx="1.2" />
      <path d="m4.6 6.6 1.2 1.2 2-2.4" />
      <rect x="3.5" y="15" width="5" height="5" rx="1.2" />
      <path d="M12 6.5h8.5M12 17.5h8.5" />
    </>
  ),
  music: (
    <>
      <path d="M9.5 17V5l9.5-2v12M9.5 8.5l9.5-2" />
      <circle cx="7" cy="17" r="2.6" fill="currentColor" />
      <circle cx="16.5" cy="15" r="2.6" fill="currentColor" />
    </>
  ),
  reading: <path d="M12 6v14M12 6 3.5 4.5V18l8.5 2 8.5-2V4.5L12 6" />,
  device: (
    <>
      <rect x="5.5" y="2.5" width="13" height="19" rx="3" />
      <rect x="8" y="5.5" width="8" height="10" rx="1" />
      <path d="M11 18.5h2" />
    </>
  ),
  settings: (
    <>
      <path d="M3.5 6h17M3.5 12h17M3.5 18h17" />
      <circle cx="15.5" cy="6" r="2.4" fill="var(--icon-knob, #fff)" />
      <circle cx="8" cy="12" r="2.4" fill="var(--icon-knob, #fff)" />
      <circle cx="13" cy="18" r="2.4" fill="var(--icon-knob, #fff)" />
    </>
  ),
  cloud: <path d="M7.5 18.5h10a3.5 3.5 0 0 0 .4-7A5.5 5.5 0 0 0 7.3 10a4.3 4.3 0 0 0 .2 8.5Z" />,
  backup: (
    <>
      <path d="M4 12a8 8 0 1 0 2.4-5.7" />
      <path d="M4 3.5v4h4M12 8v4.5l3 2" />
    </>
  ),
  link: (
    <>
      <path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1" />
      <path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1" />
    </>
  ),
  pass: (
    <>
      <path d="M3 7.5A1.5 1.5 0 0 1 4.5 6h15A1.5 1.5 0 0 1 21 7.5v2a2.5 2.5 0 0 0 0 5v2a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 16.5v-2a2.5 2.5 0 0 0 0-5Z" />
      <path d="M15 8v1.5M15 11.25v1.5M15 14.5V16" />
    </>
  ),
  plus: <path d="M12 5v14M5 12h14" />,
  'chevron-right': <path d="m9.5 5.5 6.5 6.5-6.5 6.5" />,
  'chevron-left': <path d="M14.5 5.5 8 12l6.5 6.5" />,
  wifi: (
    <>
      <path d="M2.5 9a14 14 0 0 1 19 0M5.5 12.5a9.5 9.5 0 0 1 13 0M8.6 16a5 5 0 0 1 6.8 0" />
      <circle cx="12" cy="19" r="1.3" fill="currentColor" />
    </>
  ),
  battery: (
    <>
      <rect x="2.5" y="7" width="17" height="10" rx="2.5" />
      <path d="M21.5 10.5v3" />
      <rect x="5" y="9.5" width="9" height="5" rx="1" fill="currentColor" stroke="none" />
    </>
  ),
  trash: <path d="M4 6.5h16M9.5 6.5 10 3.5h4l.5 3M6 6.5l1 14h10l1-14M10.5 10.5v6M13.5 10.5v6" />,
  share: <path d="M12 15V3.5M7.5 8 12 3.5 16.5 8M5 12v7.5h14V12" />,
  sync: (
    <>
      <path d="M20 12a8 8 0 0 1-14.3 4.9M4 12a8 8 0 0 1 14.3-4.9" />
      <path d="M18.5 3v4.5H14M5.5 21v-4.5H10" />
    </>
  ),
  user: (
    <>
      <circle cx="12" cy="8" r="4" />
      <path d="M4 20.5a8 8 0 0 1 16 0" />
    </>
  ),
  card: (
    <>
      <rect x="2.5" y="5" width="19" height="14" rx="2.5" />
      <path d="M2.5 9.5h19M6 15h4" />
    </>
  ),
  lock: (
    <>
      <rect x="5" y="10.5" width="14" height="11" rx="2" />
      <path d="M7.5 10.5V8a4.5 4.5 0 0 1 9 0v2.5" />
    </>
  ),
  mic: (
    <>
      <rect x="8.5" y="2.5" width="7" height="12" rx="3.5" />
      <path d="M5 11a7 7 0 0 0 14 0M12 18v3.5M8.5 21.5h7" />
    </>
  ),
  check: <path d="m4.5 12.5 5 5 10-11" />,
  upload: <path d="M12 16V4M7 9l5-5 5 5M4.5 15.5V20h15v-4.5" />,
  download: <path d="M12 4v12M7 11l5 5 5-5M4.5 15.5V20h15v-4.5" />,
  external: <path d="M14 4h6v6M20 4l-9 9M18 14v6H4V6h6" />,
  info: (
    <>
      <circle cx="12" cy="12" r="9.5" />
      <path d="M12 11v6" />
      <circle cx="12" cy="7.5" r="1.2" fill="currentColor" />
    </>
  ),
  qr: (
    <>
      <rect x="3.5" y="3.5" width="7" height="7" rx="1" />
      <rect x="13.5" y="3.5" width="7" height="7" rx="1" />
      <rect x="3.5" y="13.5" width="7" height="7" rx="1" />
      <path d="M14 14h2.5v2.5M20 14v.01M14 20h.01M17 20h3v-3" />
    </>
  ),
  phone: (
    <>
      <rect x="6.5" y="2.5" width="11" height="19" rx="2.5" />
      <path d="M10.5 18.5h3" />
    </>
  ),
  code: <path d="m8.5 7-5 5 5 5M15.5 7l5 5-5 5" />,
  logout: <path d="M14.5 4H19v16h-4.5M10 8l-4 4 4 4M6 12h10" />,
  sparkle: <path d="M12 3.5 13.8 10 20.5 12l-6.7 2L12 20.5 10.2 14 3.5 12l6.7-2Z" />,
}

export function Icon({
  name,
  size = 22,
  stroke = 1.8,
  className,
  title,
}: {
  name: IconName
  size?: number
  stroke?: number
  className?: string
  title?: string
}) {
  return (
    <svg
      className={className ? `icon ${className}` : 'icon'}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={stroke}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden={title ? undefined : true}
      role={title ? 'img' : undefined}
    >
      {title ? <title>{title}</title> : null}
      {paths[name]}
    </svg>
  )
}
