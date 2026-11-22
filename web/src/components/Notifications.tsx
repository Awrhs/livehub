import { useCallback, useEffect, useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { get, post, type Notification } from '../api'
import { useI18n } from '../i18n'

// 顶栏通知铃铛：每 30 秒拉取一次；打开面板时标记全部已读
export default function NotificationBell() {
  const { t, ago } = useI18n()
  const [list, setList] = useState<Notification[]>([])
  const [unread, setUnread] = useState(0)
  const [open, setOpen] = useState(false)
  const ref = useRef<HTMLDivElement>(null)

  const load = useCallback(async () => {
    try {
      const r = await get<{ notifications: Notification[]; unread: number }>('/me/notifications?limit=20')
      setList(r.notifications)
      setUnread(r.unread)
    } catch {
      // 忽略
    }
  }, [])

  useEffect(() => {
    void load()
    const id = window.setInterval(load, 30000)
    return () => window.clearInterval(id)
  }, [load])

  useEffect(() => {
    const close = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', close)
    return () => document.removeEventListener('mousedown', close)
  }, [])

  const toggle = async () => {
    const next = !open
    setOpen(next)
    if (next && unread > 0) {
      setUnread(0)
      try {
        await post('/me/notifications/read')
      } catch {
        // 忽略
      }
    }
  }

  const text = (n: Notification) => {
    switch (n.kind) {
      case 'live':
        return t('notify.live', { name: n.channel_name ?? '', title: n.body })
      case 'stream_approved':
        return t('notify.stream_approved')
      case 'stream_rejected':
        return t('notify.stream_rejected')
      default:
        return n.body
    }
  }

  return (
    <div className="menu" ref={ref}>
      <button className="ghost" onClick={toggle} aria-label={t('notify.title')} data-testid="notify-bell" style={{ position: 'relative' }}>
        🔔
        {unread > 0 && (
          <span className="badge live" style={{ position: 'absolute', top: -2, right: -4, padding: '0 5px' }} data-testid="notify-count">
            {unread > 99 ? '99+' : unread}
          </span>
        )}
      </button>
      {open && (
        <div className="dropdown" style={{ width: 320, maxHeight: 420, overflowY: 'auto' }}>
          <div className="who">{t('notify.title')}</div>
          <hr />
          {list.length === 0 && <div className="who">{t('notify.empty')}</div>}
          {list.map((n) =>
            n.channel_slug ? (
              <Link key={n.id} to={'/' + n.channel_slug} onClick={() => setOpen(false)} style={{ display: 'block' }}>
                <div style={{ fontWeight: n.read_at ? 400 : 600 }}>{text(n)}</div>
                <div className="dim" style={{ fontSize: 12 }}>
                  {ago(n.created_at)}
                </div>
              </Link>
            ) : (
              <Link key={n.id} to="/dashboard" onClick={() => setOpen(false)} style={{ display: 'block' }}>
                <div style={{ fontWeight: n.read_at ? 400 : 600 }}>{text(n)}</div>
                <div className="dim" style={{ fontSize: 12 }}>
                  {ago(n.created_at)}
                </div>
              </Link>
            ),
          )}
        </div>
      )}
    </div>
  )
}
