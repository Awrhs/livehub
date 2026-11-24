import { useCallback, useEffect, useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { get, post, type Notification } from '../api'
import { useI18n } from '../i18n'

const canDesktop = typeof window !== 'undefined' && 'Notification' in window

// 顶栏通知铃铛：通过 SSE 实时接收新通知（断线时浏览器自动重连），并每 60 秒兜底拉取；
// 打开面板时标记全部已读。用户授权后，新通知同时以桌面通知弹出。
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

  const textRef = useRef<(n: Notification) => string>(() => '')
  const [perm, setPerm] = useState(canDesktop ? Notification.permission : 'denied')

  useEffect(() => {
    void load()
    const id = window.setInterval(load, 60000)
    const es = new EventSource('/api/v1/me/events')
    es.onmessage = async () => {
      try {
        const r = await get<{ notifications: Notification[]; unread: number }>('/me/notifications?limit=20')
        setList(r.notifications)
        setUnread(r.unread)
        const n = r.notifications[0]
        if (n && canDesktop && Notification.permission === 'granted') {
          const d = new Notification(document.title, { body: textRef.current(n), tag: 'livehub-' + n.id })
          d.onclick = () => {
            window.focus()
            window.location.href = n.channel_slug ? '/' + n.channel_slug : '/dashboard'
          }
        }
      } catch {
        // 忽略
      }
    }
    return () => {
      window.clearInterval(id)
      es.close()
    }
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
      case 'disk_low':
        return t('notify.disk_low', { body: n.body })
      default:
        return n.body
    }
  }

  textRef.current = text

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
          {perm === 'default' && (
            <button
              onClick={async () => {
                setPerm(await Notification.requestPermission())
              }}
            >
              🔔 {t('notify.enableDesktop')}
            </button>
          )}
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
