import { useCallback, useEffect, useState } from 'react'
import { useParams } from 'react-router-dom'
import { ApiError, del, get, post, type Channel, type VOD } from '../api'
import VodCard from '../components/VodCard'
import Avatar from '../components/Avatar'
import { useSession } from '../session'
import { Link } from 'react-router-dom'
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
  const { t, duration, errText } = useI18n()
  const { me } = useSession()
  const [followers, setFollowers] = useState(0)
  const [following, setFollowing] = useState(false)
  const [tab, setTab] = useState<'live' | 'vods'>('live')
  const [vods, setVods] = useState<VOD[] | null>(null)
  const [notice, setNotice] = useState('')
  const [ch, setCh] = useState<Channel | null>(null)
  const [owner, setOwner] = useState<Owner | null>(null)
  const [missing, setMissing] = useState(false)
  const [viewers, setViewers] = useState(0)
  const [now, setNow] = useState(Date.now() / 1000)

  const load = useCallback(async () => {
    try {
      const r = await get<{ channel: Channel; owner: Owner; followers: number; following: boolean }>(
        '/channels/' + encodeURIComponent(slug),
      )
      setCh(r.channel)
      setFollowers(r.followers)
      setFollowing(r.following)
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
    if (tab !== 'vods') return
    get<{ vods: VOD[] }>('/channels/' + encodeURIComponent(slug) + '/vods')
      .then((r) => setVods(r.vods))
      .catch(() => setVods([]))
  }, [tab, slug])

  useEffect(() => {
    const id = window.setInterval(() => setNow(Date.now() / 1000), 1000)
    return () => window.clearInterval(id)
  }, [])

  const toggleFollow = async () => {
    try {
      const r = following
        ? await del<{ following: boolean; followers: number }>('/channels/' + slug + '/follow')
        : await post<{ following: boolean; followers: number }>('/channels/' + slug + '/follow')
      setFollowing(r.following)
      setFollowers(r.followers)
    } catch (e) {
      setNotice(errText(e))
    }
  }

  const report = async () => {
    const reason = window.prompt(t('report.prompt'))
    if (!reason) return
    try {
      await post('/channels/' + slug + '/report', { reason })
      setNotice(t('report.done'))
    } catch (e) {
      setNotice(errText(e))
    }
  }

  if (missing) return <NotFound text={t('channel.notFound')} />
  if (!ch || !owner) return <p className="dim" style={{ padding: 20 }}>{t('common.loading')}</p>

  return (
    <div className="watch">
      <div className="stage">
        {tab === 'vods' ? (
          <div style={{ padding: 20 }}>
            {vods === null ? (
              <p className="dim">{t('common.loading')}</p>
            ) : vods.length === 0 ? (
              <p className="dim">{t('vod.empty')}</p>
            ) : (
              <div className="grid">
                {vods.map((v) => (
                  <VodCard key={v.id} v={v} showChannel={false} />
                ))}
              </div>
            )}
          </div>
        ) : ch.is_live && ch.playback ? (
          <Player hls={ch.playback.hls} flv={ch.playback.flv} abr={ch.playback.abr} webrtc={ch.playback.webrtc} />
        ) : (
          <div className="player">
            <div className="overlay" data-testid="offline">
              <h2>{t('channel.offline', { name: ch.display_name })}</h2>
              <p className="dim">{t('channel.offlineHint')}</p>
            </div>
          </div>
        )}
        <div className="info">
          <Avatar name={ch.display_name} url={ch.avatar_url} size="lg" />
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
              <span className="dim">{t('follow.followers', { n: followers })}</span>
            </div>
            <div className="tabs" style={{ marginTop: 12, marginBottom: 0 }}>
              <button className={tab === 'live' ? 'active' : ''} onClick={() => setTab('live')}>
                {t('vod.liveTab')}
              </button>
              <button className={tab === 'vods' ? 'active' : ''} onClick={() => setTab('vods')} data-testid="tab-vods">
                {t('vod.title')}
              </button>
            </div>
          </div>
          <div className="row" style={{ alignSelf: 'flex-start' }}>
            {me ? (
              me.id !== ch.user_id && (
                <button className={following ? '' : 'primary'} onClick={toggleFollow} data-testid="follow-btn">
                  {following ? '♥ ' + t('follow.unfollow') : '♡ ' + t('follow.follow')}
                </button>
              )
            ) : (
              <Link to={'/login?next=/' + ch.slug} className="btn primary">
                {t('follow.loginToFollow')}
              </Link>
            )}
            {me && me.id !== ch.user_id && (
              <button className="ghost small" onClick={report} title={t('report.btn')}>
                ⚑
              </button>
            )}
          </div>
        </div>
        {notice && (
          <div className="banner" style={{ margin: '0 20px 16px' }} onClick={() => setNotice('')}>
            {notice}
          </div>
        )}
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
          onLive: (live) => {
            // 状态与当前显示不一致时重新加载（开播后取得播放地址）
            if (live !== ch.is_live) void load()
          },
          onViewers: setViewers,
          onChannel: (title, category) => setCh((c) => (c ? { ...c, title, category } : c)),
        }}
      />
    </div>
  )
}
