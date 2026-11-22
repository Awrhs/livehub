import { useEffect, useState } from 'react'
import { Link, useNavigate, useParams } from 'react-router-dom'
import { ApiError, del, get, patch, type VOD } from '../api'
import NotFound from './NotFound'
import { useI18n } from '../i18n'
import { useSession } from '../session'

// 录像回放页
export default function Video() {
  const { id = '' } = useParams()
  const { t, ago, errText } = useI18n()
  const { me } = useSession()
  const nav = useNavigate()
  const [v, setV] = useState<VOD | null>(null)
  const [missing, setMissing] = useState(false)
  const [err, setErr] = useState('')

  useEffect(() => {
    setV(null)
    setMissing(false)
    get<VOD>('/vods/' + encodeURIComponent(id))
      .then((r) => {
        setV(r)
        document.title = r.title
      })
      .catch((e) => {
        if (e instanceof ApiError && e.status === 404) setMissing(true)
      })
  }, [id])

  if (missing) return <NotFound text={t('vod.notFound')} />
  if (!v) return <p className="dim">{t('common.loading')}</p>
  const canEdit = me && (me.username.toLowerCase() === v.channel_slug.toLowerCase() || me.role === 'admin')

  const rename = async () => {
    const title = window.prompt(t('vod.rename'), v.title)
    if (!title) return
    try {
      setV(await patch<VOD>('/vods/' + v.id, { title }))
    } catch (e) {
      setErr(errText(e))
    }
  }
  const remove = async () => {
    if (!window.confirm(t('vod.deleteConfirm'))) return
    try {
      await del('/vods/' + v.id)
      nav('/' + v.channel_slug)
    } catch (e) {
      setErr(errText(e))
    }
  }

  return (
    <div style={{ maxWidth: 1100, margin: '0 auto' }}>
      <div className="player">
        <video src={v.url} controls playsInline autoPlay poster={v.thumbnail} data-testid="vod-video" />
      </div>
      <div className="info" style={{ padding: '16px 0', display: 'flex', gap: 14 }}>
        <span className="avatar lg">{v.display_name.slice(0, 1)}</span>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: 18, fontWeight: 600 }}>{v.title}</div>
          <div>
            <Link to={'/' + v.channel_slug}>{v.display_name}</Link>
          </div>
          <div className="dim">
            {t('vod.views', { n: v.views })} · {ago(v.created_at)} {v.category && <span className="badge">{v.category}</span>}
          </div>
        </div>
        {canEdit && (
          <div className="row">
            <button onClick={rename}>{t('vod.rename')}</button>
            <button className="danger" onClick={remove}>
              {t('common.delete')}
            </button>
          </div>
        )}
      </div>
      {err && <div className="error">{err}</div>}
    </div>
  )
}
