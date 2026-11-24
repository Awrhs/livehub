import { Link } from 'react-router-dom'
import type { Channel } from '../api'
import { useI18n } from '../i18n'
import Avatar from './Avatar'

export default function ChannelCard({ ch }: { ch: Channel }) {
  const { t, ago } = useI18n()
  return (
    <Link to={'/' + ch.slug} className="ch-card" data-testid={'channel-card-' + ch.slug}>
      <div className="thumb">
        {ch.thumbnail ? <img src={ch.thumbnail} alt="" loading="lazy" /> : <span className="initial">{ch.display_name.slice(0, 1)}</span>}
        {ch.is_live && <span className="badge live">{t('common.live')}</span>}
        {ch.is_live && <span className="viewers">{t('common.viewers', { n: ch.viewers ?? 0 })}</span>}
      </div>
      <div className="meta">
        <Avatar name={ch.display_name} url={ch.avatar_url} />
        <div style={{ minWidth: 0 }}>
          <div className="title">{ch.title}</div>
          <div className="sub">{ch.display_name}</div>
          <div className="sub">
            {ch.category && <span className="badge">{ch.category}</span>}{' '}
            {!ch.is_live && (ch.last_live_at ? t('home.lastLive', { t: ago(ch.last_live_at) }) : t('home.neverLive'))}
          </div>
        </div>
      </div>
    </Link>
  )
}
