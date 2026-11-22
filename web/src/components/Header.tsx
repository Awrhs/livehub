import { useEffect, useRef, useState, type FormEvent } from 'react'
import { Link, NavLink, useNavigate } from 'react-router-dom'
import { useSession } from '../session'
import { useI18n } from '../i18n'
import NotificationBell from './Notifications'

export default function Header() {
  const { site, me, logout } = useSession()
  const { t, lang, setLang } = useI18n()
  const nav = useNavigate()
  const [q, setQ] = useState('')
  const [open, setOpen] = useState(false)
  const menuRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const close = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', close)
    return () => document.removeEventListener('mousedown', close)
  }, [])

  const search = (e: FormEvent) => {
    e.preventDefault()
    const v = q.trim()
    nav(v ? '/search?q=' + encodeURIComponent(v) : '/')
  }

  return (
    <header className="header">
      <Link to="/" className="brand">
        <img src="/favicon.svg" alt="" />
        <span>{site?.name || 'LiveHub'}</span>
      </Link>
      <nav>
        <NavLink to="/" end>
          {t('nav.home')}
        </NavLink>
        {me && <NavLink to="/following">{t('nav.following')}</NavLink>}
        {me && <NavLink to="/dashboard">{t('nav.dashboard')}</NavLink>}
        {me?.role === 'admin' && <NavLink to="/admin">{t('nav.admin')}</NavLink>}
      </nav>
      <form className="search" onSubmit={search}>
        <input type="search" value={q} onChange={(e) => setQ(e.target.value)} placeholder={t('nav.search')} />
      </form>
      <div className="right">
        <button className="ghost small" onClick={() => setLang(lang === 'zh' ? 'en' : 'zh')} title={t('settings.language')}>
          {lang === 'zh' ? 'EN' : '中文'}
        </button>
        {me && <NotificationBell />}
        {me ? (
          <div className="menu" ref={menuRef}>
            <button className="ghost" onClick={() => setOpen((v) => !v)} aria-label="menu" data-testid="user-menu">
              <span className="avatar" style={{ width: 30, height: 30 }}>
                {me.display_name.slice(0, 1)}
              </span>
            </button>
            {open && (
              <div className="dropdown" onClick={() => setOpen(false)}>
                <div className="who">
                  {me.display_name} <span className="dim">@{me.username}</span>
                </div>
                <hr />
                <Link to={'/' + me.username}>{t('nav.myChannel')}</Link>
                <Link to="/following">{t('nav.following')}</Link>
                <Link to="/dashboard">{t('nav.dashboard')}</Link>
                {me.role === 'admin' && <Link to="/admin">{t('nav.admin')}</Link>}
                <Link to="/settings">{t('nav.settings')}</Link>
                <hr />
                <button
                  onClick={async () => {
                    await logout()
                    nav('/')
                  }}
                >
                  {t('nav.logout')}
                </button>
              </div>
            )}
          </div>
        ) : (
          <>
            <Link to="/login" className="btn small">
              {t('nav.login')}
            </Link>
            {site?.registration_mode !== 'closed' && (
              <Link to="/register" className="btn primary small">
                {t('nav.register')}
              </Link>
            )}
          </>
        )}
      </div>
    </header>
  )
}
