import { useEffect, useState, type FormEvent } from 'react'
import { Link, Navigate } from 'react-router-dom'
import { del, get, patch, post, type Me } from '../api'
import CopyField from '../components/Copy'
import { useI18n } from '../i18n'
import { useSession } from '../session'

interface ModUser {
  id: number
  username: string
  display_name: string
}

// 频道房管管理
function Moderators({ slug }: { slug: string }) {
  const { t, errText } = useI18n()
  const [list, setList] = useState<ModUser[]>([])
  const [name, setName] = useState('')
  const [err, setErr] = useState('')
  useEffect(() => {
    get<{ moderators: ModUser[] }>('/channels/' + slug + '/moderators')
      .then((r) => setList(r.moderators))
      .catch(() => {})
  }, [slug])
  const add = async (e: FormEvent) => {
    e.preventDefault()
    setErr('')
    try {
      const r = await post<{ moderators: ModUser[] }>('/channels/' + slug + '/moderators', { username: name.trim() })
      setList(r.moderators)
      setName('')
    } catch (e) {
      setErr(errText(e))
    }
  }
  const remove = async (id: number) => {
    try {
      const r = await del<{ moderators: ModUser[] }>('/channels/' + slug + '/moderators/' + id)
      setList(r.moderators)
    } catch (e) {
      setErr(errText(e))
    }
  }
  return (
    <div className="card">
      <h2>{t('mod.title')}</h2>
      <p className="hint">{t('mod.hint')}</p>
      <form className="row" onSubmit={add} style={{ marginBottom: 12 }}>
        <input value={name} onChange={(e) => setName(e.target.value)} placeholder={t('mod.usernamePlaceholder')} required />
        <button type="submit">{t('mod.add')}</button>
      </form>
      {err && <div className="error">{err}</div>}
      {list.map((m) => (
        <div className="row" key={m.id} style={{ padding: '6px 0' }}>
          <span>
            {m.display_name} <span className="dim">@{m.username}</span>
          </span>
          <span className="spacer" />
          <button className="small" onClick={() => void remove(m.id)}>
            {t('mod.remove')}
          </button>
        </div>
      ))}
    </div>
  )
}

export default function Dashboard() {
  const { t, errText } = useI18n()
  const { me, setMe, refresh } = useSession()
  const [title, setTitle] = useState('')
  const [category, setCategory] = useState('')
  const [description, setDescription] = useState('')
  const [record, setRecord] = useState(true)
  const [note, setNote] = useState('')
  const [msg, setMsg] = useState('')
  const [err, setErr] = useState('')
  const [newKey, setNewKey] = useState<Me['new_key'] | null>(null)

  useEffect(() => {
    if (!me) return
    setTitle(me.channel.title)
    setCategory(me.channel.category)
    setDescription(me.channel.description)
    setRecord(me.channel.record_enabled)
    setNote(me.stream_request_note)
    // 仅在切换账号时初始化表单
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [me?.id])

  // 每 10 秒刷新直播状态
  useEffect(() => {
    const id = window.setInterval(() => void refresh(), 10000)
    return () => window.clearInterval(id)
  }, [refresh])

  if (!me) return <Navigate to="/login?next=/dashboard" replace />

  const flash = (m: string) => {
    setMsg(m)
    setErr('')
    window.setTimeout(() => setMsg(''), 2500)
  }

  const saveChannel = async (e: FormEvent) => {
    e.preventDefault()
    try {
      const m = await patch<Me>('/me/channel', { title, category, description, record_enabled: record })
      setMe(m)
      flash(t('common.saved'))
    } catch (e) {
      setErr(errText(e))
    }
  }

  const resetKey = async () => {
    if (me.channel.has_stream_key && !window.confirm(t('dash.resetConfirm'))) return
    try {
      const m = await post<Me>('/me/channel/key')
      setNewKey(m.new_key ?? null)
      setMe(m)
    } catch (e) {
      setErr(errText(e))
    }
  }

  const request = async (e: FormEvent) => {
    e.preventDefault()
    try {
      setMe(await post<Me>('/me/stream-request', { note }))
    } catch (e) {
      setErr(errText(e))
    }
  }

  const ch = me.channel
  return (
    <div>
      <h1>{t('dash.title')}</h1>
      <div className="card row" style={{ flexWrap: 'wrap' }}>
        <strong>{t('dash.status')}：</strong>
        {ch.is_live ? <span className="badge live">{t('dash.live')}</span> : <span className="badge">{t('dash.offline')}</span>}
        <span className="spacer" />
        <Link to={'/' + ch.slug} className="btn">
          {t('dash.viewChannel')}
        </Link>
      </div>
      {msg && <div className="success">{msg}</div>}
      {err && <div className="error">{err}</div>}

      <div className="two-col">
        <div>
          {me.can_stream ? (
            <div className="card">
              <h2>{t('dash.streamSetup')}</h2>
              <label className="field">
                <span>{t('dash.server')}</span>
                <CopyField value={me.ingest.rtmp} testId="ingest-url" />
              </label>
              {newKey ? (
                <>
                  <div className="banner warn">{t('dash.keyOnce')}</div>
                  <label className="field">
                    <span>{t('dash.streamKey')}</span>
                    <CopyField value={newKey.publish_key} testId="publish-key" />
                  </label>
                  {newKey.srt_url && (
                    <label className="field">
                      <span>{t('dash.srt')}</span>
                      <CopyField value={newKey.srt_url} />
                    </label>
                  )}
                </>
              ) : (
                <p className="hint">{ch.has_stream_key ? t('dash.keyHidden') : t('dash.keyNone')}</p>
              )}
              <p className="hint">{t('dash.keyWarning')}</p>
              <p className="hint">{t('dash.obsGuide')}</p>
              <button className={ch.has_stream_key ? 'danger' : 'primary'} onClick={resetKey} data-testid="generate-key">
                {ch.has_stream_key ? t('dash.resetKey') : t('dash.generateKey')}
              </button>
            </div>
          ) : (
            <div className="card" data-testid="stream-request">
              <h2>{t('dash.needApproval')}</h2>
              {me.banned ? (
                <p className="error">{t('dash.banned')}</p>
              ) : me.stream_status === 'pending' ? (
                <div className="banner warn">{t('dash.pending')}</div>
              ) : (
                <form onSubmit={request}>
                  {me.stream_status === 'rejected' && <div className="banner warn">{t('dash.rejected')}</div>}
                  <label className="field">
                    <span>{t('dash.requestNote')}</span>
                    <textarea value={note} onChange={(e) => setNote(e.target.value)} maxLength={500} />
                  </label>
                  <button className="primary" type="submit">
                    {t('dash.requestBtn')}
                  </button>
                </form>
              )}
            </div>
          )}
        </div>
        <div>
        <form className="card" onSubmit={saveChannel}>
          <h2>{t('dash.channelInfo')}</h2>
          <label className="field">
            <span>{t('dash.channelTitle')}</span>
            <input value={title} onChange={(e) => setTitle(e.target.value)} maxLength={80} required data-testid="channel-title-input" />
          </label>
          <label className="field">
            <span>{t('dash.category')}</span>
            <input value={category} onChange={(e) => setCategory(e.target.value)} maxLength={32} />
          </label>
          <label className="field">
            <span>{t('dash.description')}</span>
            <textarea value={description} onChange={(e) => setDescription(e.target.value)} maxLength={2000} />
          </label>
          <label className="check">
            <input type="checkbox" checked={record} onChange={(e) => setRecord(e.target.checked)} />
            {t('dash.record')}
          </label>
          <button className="primary" type="submit">
            {t('common.save')}
          </button>
        </form>
        <Moderators slug={ch.slug} />
        </div>
      </div>
    </div>
  )
}
