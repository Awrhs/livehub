import { useEffect, useRef, useState, type FormEvent } from 'react'
import { Link } from 'react-router-dom'
import { del, post, type ChatMessage } from '../api'
import { useI18n } from '../i18n'
import type { MsgKey } from '../i18n/zh'

interface ChatMe {
  id: number
  username: string
  display_name: string
  role: string
  can_moderate: boolean
}

export interface ChatEvents {
  onLive?: (live: boolean) => void
  onViewers?: (n: number) => void
  onChannel?: (title: string, category: string) => void
}

const EMOJIS = ['😀', '😂', '🤣', '😍', '🥰', '😎', '🤔', '😮', '😭', '😡', '👍', '👎', '👏', '🙏', '🔥', '❤️', '🎉', '💯', '🚀', '👀', '🤝', '😴', '🍿', '🎮']

const MENTION_RE = /(@[A-Za-z][A-Za-z0-9_]{2,23})/g

// 渲染消息正文：@用户名 加粗显示
function renderBody(body: string) {
  return body.split(MENTION_RE).map((part, i) =>
    i % 2 === 1 ? (
      <strong key={i} className="mention-tag">
        {part}
      </strong>
    ) : (
      part
    ),
  )
}

// 按用户名生成稳定的昵称颜色
function nameColor(name: string): string {
  let h = 0
  for (let i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) % 360
  return `hsl(${h}, 70%, 65%)`
}

export default function Chat({ slug, events }: { slug: string; events: ChatEvents }) {
  const { t } = useI18n()
  const [msgs, setMsgs] = useState<ChatMessage[]>([])
  const [me, setMe] = useState<ChatMe | null>(null)
  const [connected, setConnected] = useState(false)
  const [everConnected, setEverConnected] = useState(false)
  const [slowMode, setSlowMode] = useState(0)
  const [text, setText] = useState('')
  const [err, setErr] = useState('')
  const [emojiOpen, setEmojiOpen] = useState(false)
  const wsRef = useRef<WebSocket | null>(null)
  const listRef = useRef<HTMLDivElement>(null)
  const stickRef = useRef(true)
  const eventsRef = useRef(events)
  eventsRef.current = events

  useEffect(() => {
    let closed = false
    let attempt = 0
    let timer: number | undefined
    const connect = () => {
      const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
      const ws = new WebSocket(`${proto}//${location.host}/api/v1/channels/${encodeURIComponent(slug)}/chat/ws`)
      wsRef.current = ws
      ws.onopen = () => {
        attempt = 0
        setConnected(true)
        setEverConnected(true)
      }
      ws.onmessage = (ev) => {
        let m: Record<string, unknown>
        try {
          m = JSON.parse(ev.data as string)
        } catch {
          return
        }
        switch (m.type) {
          case 'hello':
            setMsgs((m.history as ChatMessage[]) || [])
            setMe((m.me as ChatMe) || null)
            setSlowMode((m.slow_mode as number) || 0)
            eventsRef.current.onViewers?.(m.viewers as number)
            // 连接建立时同步一次直播状态：页面加载与聊天连接之间开播/下播的事件不会丢失
            eventsRef.current.onLive?.(m.is_live as boolean)
            break
          case 'msg':
            setMsgs((prev) => [...prev.slice(-199), m.message as ChatMessage])
            break
          case 'delete':
            setMsgs((prev) => prev.filter((x) => x.id !== m.id))
            break
          case 'viewers':
            eventsRef.current.onViewers?.(m.count as number)
            break
          case 'live':
            eventsRef.current.onLive?.(m.is_live as boolean)
            break
          case 'channel':
            eventsRef.current.onChannel?.(m.title as string, m.category as string)
            break
          case 'error': {
            const k = ('err.' + m.code) as MsgKey
            const txt = t(k)
            setErr(txt === k ? String(m.message) : txt)
            window.setTimeout(() => setErr(''), 4000)
            break
          }
        }
      }
      ws.onclose = () => {
        setConnected(false)
        if (closed) return
        // 指数退避重连（最长 15 秒）
        attempt++
        timer = window.setTimeout(connect, Math.min(15000, 500 * 2 ** attempt))
      }
    }
    connect()
    return () => {
      closed = true
      window.clearTimeout(timer)
      wsRef.current?.close()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [slug])

  // 新消息自动滚动到底部（用户向上翻阅时不打扰）
  useEffect(() => {
    const el = listRef.current
    if (el && stickRef.current) el.scrollTop = el.scrollHeight
  }, [msgs])

  const send = (e: FormEvent) => {
    e.preventDefault()
    const body = text.trim()
    if (!body || !wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) return
    wsRef.current.send(JSON.stringify({ type: 'send', body }))
    setText('')
    stickRef.current = true
  }

  const ban = async (userId: number, durationSec: number) => {
    try {
      await post('/channels/' + encodeURIComponent(slug) + '/bans', { user_id: userId, duration_sec: durationSec })
    } catch {
      // 失败时（如目标为频道主）静默
    }
  }

  const remove = async (id: number) => {
    try {
      await del('/chat/messages/' + id)
    } catch {
      // 删除事件会经 WebSocket 广播；失败时无需处理
    }
  }

  return (
    <aside className="chat" data-testid="chat">
      <div className="head">
        <span>{t('chat.title')}</span>
        {slowMode > 0 && <span className="badge">{t('chat.slowMode', { n: slowMode })}</span>}
      </div>
      <div
        className="msgs"
        ref={listRef}
        onScroll={(e) => {
          const el = e.currentTarget
          stickRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40
        }}
      >
        {msgs.length === 0 && connected && <div className="status">{t('chat.empty')}</div>}
        {msgs.map((m) => (
          <div
            className={
              'msg' + (me && m.user.id !== me.id && m.body.toLowerCase().includes('@' + me.username.toLowerCase()) ? ' mentioned' : '')
            }
            key={m.id}
            data-testid="chat-msg"
          >
            {m.user.role === 'admin' && <span className="badge accent" style={{ marginRight: 4 }}>A</span>}
            {m.user.role === 'moderator' && <span className="badge ok" style={{ marginRight: 4 }}>M</span>}
            <span className="name" style={{ color: nameColor(m.user.username) }}>
              {m.user.display_name}
            </span>
            <span>{renderBody(m.body)}</span>
            {me?.can_moderate && (
              <span className="del" style={{ gap: 4 }}>
                {m.user.id !== me.id && (
                  <>
                    <button className="small" onClick={() => ban(m.user.id, 600)} title={t('mod.timeout10')}>
                      ⏱
                    </button>
                    <button className="small" onClick={() => ban(m.user.id, 0)} title={t('mod.ban')}>
                      ⛔
                    </button>
                  </>
                )}
                <button className="small danger" onClick={() => remove(m.id)} title={t('common.delete')}>
                  ×
                </button>
              </span>
            )}
          </div>
        ))}
      </div>
      {!connected && <div className="status">{everConnected ? t('chat.disconnected') : t('chat.connecting')}</div>}
      {err && <div className="status error">{err}</div>}
      {me ? (
        <form onSubmit={send} style={{ position: 'relative' }}>
          <button type="button" className="ghost" onClick={() => setEmojiOpen((v) => !v)} aria-label="emoji" data-testid="emoji-btn">
            😀
          </button>
          {emojiOpen && (
            <div className="emoji-picker">
              {EMOJIS.map((e) => (
                <button
                  type="button"
                  key={e}
                  className="ghost"
                  onClick={() => {
                    setText((v) => v + e)
                    setEmojiOpen(false)
                  }}
                >
                  {e}
                </button>
              ))}
            </div>
          )}
          <input
            value={text}
            onChange={(e) => setText(e.target.value)}
            placeholder={t('chat.placeholder')}
            maxLength={300}
            data-testid="chat-input"
          />
          <button className="primary" type="submit" disabled={!connected || !text.trim()}>
            {t('chat.send')}
          </button>
        </form>
      ) : (
        <div className="login-hint">
          <Link to="/login">{t('chat.loginToChat')}</Link>
        </div>
      )}
    </aside>
  )
}
