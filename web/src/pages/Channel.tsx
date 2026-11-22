import { useCallback, useEffect, useState } from 'react'
import { useParams } from 'react-router-dom'
import { ApiError, get, type Channel } from '../api'
import Player from '../components/Player'
import Chat from '../components/Chat'
import NotFound from './NotFound'
import { useI18n } from '../i18n'

interface Owner {
  id: number
  username: string
  display_name: string
  bio: string
}

export default function ChannelPage() {
  const { slug = '' } = useParams()
  const { t, duration } = useI18n()
  const [ch, setCh] = useState<Channel | null>(null)
  const [owner, setOwner] = useState<Owner | null>(null)
  const [missing, setMissing] = useState(false)
  const [viewers, setViewers] = useState(0)
  const [now, setNow] = useState(Date.now() / 1000)

  const load = useCallback(async () => {
    try {
      const r = await get<{ channel: Channel; owner: Owner }>('/channels/' + encodeURIComponent(slug))
      setCh(r.channel)
      setOwner(r.owner)
      setViewers(r.channel.viewers ?? 0)
      document.title = `${r.channel.display_name} - ${r.channel.title}`
    } catch (e) {
      if (e instanceof ApiError && e.status === 404) setMissing(true)
    }
  }, [slug])

  useEffect(() => {
    setMissing(false)
    setCh(null)
    void load()
  }, [load])

  useEffect(() => {
    const id = window.setInterval(() => setNow(Date.now() / 1000), 1000)
    return () => window.clearInterval(id)
  }, [])

  if (missing) return <NotFound text={t('channel.notFound')} />
  if (!ch || !owner) return <p className="dim" style={{ padding: 20 }}>{t('common.loading')}</p>

  return (
    <div className="watch">
      <div className="stage">
        {ch.is_live && ch.playback ? (
          <Player hls={ch.playback.hls} flv={ch.playback.flv} abr={ch.playback.abr} />
        ) : (
          <div className="player">
            <div className="overlay" data-testid="offline">
              <h2>{t('channel.offline', { name: ch.display_name })}</h2>
              <p className="dim">{t('channel.offlineHint')}</p>
            </div>
          </div>
        )}
        <div className="info">
          <span className="avatar lg">{ch.display_name.slice(0, 1)}</span>
          <div style={{ flex: 1, minWidth: 0 }}>
            <div className="title" data-testid="channel-title">{ch.title}</div>
            <div>
              <strong>{ch.display_name}</strong> <span className="dim">@{ch.slug}</span>
            </div>
            <div className="row" style={{ marginTop: 6, flexWrap: 'wrap' }}>
              {ch.is_live ? (
                <span className="badge live" data-testid="live-badge">{t('common.live')}</span>
              ) : (
                <span className="badge">{t('common.offline')}</span>
              )}
              {ch.category && <span className="badge">{ch.category}</span>}
              {ch.is_live && <span className="dim">{t('common.viewers', { n: viewers })}</span>}
              {ch.is_live && ch.live_started_at && (
                <span className="dim">{t('channel.uptime', { t: duration(now - ch.live_started_at) })}</span>
              )}
            </div>
          </div>
        </div>
        {(ch.description || owner.bio) && (
          <div className="about">
            <div className="card">
              <h3>{t('channel.about', { name: ch.display_name })}</h3>
              {ch.description && <p style={{ whiteSpace: 'pre-wrap' }}>{ch.description}</p>}
              {owner.bio && <p className="dim" style={{ whiteSpace: 'pre-wrap' }}>{owner.bio}</p>}
            </div>
          </div>
        )}
      </div>
      <Chat
        slug={ch.slug}
        events={{
          onLive: () => void load(),
          onViewers: setViewers,
          onChannel: (title, category) => setCh((c) => (c ? { ...c, title, category } : c)),
        }}
      />
    </div>
  )
}
