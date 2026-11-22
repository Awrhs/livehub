import { Link } from 'react-router-dom'
import type { VOD } from '../api'
import { useI18n } from '../i18n'

export default function VodCard({ v, showChannel = true }: { v: VOD; showChannel?: boolean }) {
  const { t, ago, duration } = useI18n()
  return (
    <Link to={'/videos/' + v.id} className="ch-card" data-testid={'vod-card-' + v.id}>
      <div className="thumb">
        {v.thumbnail ? <img src={v.thumbnail} alt="" loading="lazy" /> : <span className="initial">▶</span>}
        <span className="viewers" style={{ left: 'auto', right: 8 }}>
          {duration(v.duration_sec)}
        </span>
      </div>
      <div className="meta">
        <div style={{ minWidth: 0 }}>
          <div className="title">{v.title}</div>
          {showChannel && <div className="sub">{v.display_name}</div>}
          <div className="sub">
            {t('vod.views', { n: v.views })} · {ago(v.created_at)}
          </div>
        </div>
      </div>
    </Link>
  )
}
