import { NavLink } from 'react-router-dom'
import { Icon, type IconName } from './Icon'

const links: { to: string; label: string; icon: IconName; end?: boolean }[] = [
  { to: '/home', label: 'Home', icon: 'home', end: true },
  { to: '/notes', label: 'Notes', icon: 'notes' },
  { to: '/lists', label: 'Lists', icon: 'lists' },
  { to: '/devices', label: 'Pocket', icon: 'device' },
  { to: '/account', label: 'Account', icon: 'user' },
]

export function BottomNav() {
  return (
    <nav className="tabbar" aria-label="Primary">
      {links.map((l) => (
        <NavLink key={l.to} to={l.to} end={l.end} className={({ isActive }) => (isActive ? 'tab active' : 'tab')}>
          <Icon name={l.icon} size={24} stroke={1.9} />
          <span className="tab__label">{l.label}</span>
        </NavLink>
      ))}
    </nav>
  )
}

export function DesktopNav() {
  return (
    <nav className="desktop-nav" aria-label="Primary">
      {links.map((l) => (
        <NavLink key={l.to} to={l.to} end={l.end} className={({ isActive }) => (isActive ? 'active' : undefined)}>
          {l.label}
        </NavLink>
      ))}
    </nav>
  )
}
