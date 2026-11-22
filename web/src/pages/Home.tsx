import { useEffect, useState } from 'react'
import { Link, useSearchParams } from 'react-router-dom'
import { get, type Channel } from '../api'
import ChannelCard from '../components/ChannelCard'
import { useI18n } from '../i18n'
import { useSession } from '../session'

interface Category {
  name: string
  live: number
}

export default function Home() {
  const { t } = useI18n()
  const { site } = useSession()
  const [params] = useSearchParams()
  const q = params.get('q') || ''
  const [category, setCategory] = useState('')
  const [channels, setChannels] = useState<Channel[] | null>(null)
  const [cats, setCats] = useState<Category[]>([])

  useEffect(() => {
    let stop = false
    const load = async () => {
      const qs = new URLSearchParams()
      if (q) qs.set('q', q)
      if (category) qs.set('category', category)
      try {
        const [c, k] = await Promise.all([
          get<{ channels: Channel[] }>('/channels?' + qs.toString()),
          get<{ categories: Category[] }>('/categories'),
        ])
        if (!stop) {
          setChannels(c.channels)
          setCats(k.categories)
        }
      } catch {
        if (!stop) setChannels([])
      }
    }
    void load()
    // 每 20 秒刷新直播列表与观众数
    const id = window.setInterval(load, 20000)
    return () => {
      stop = true
      window.clearInterval(id)
    }
  }, [q, category])

  const live = channels?.filter((c) => c.is_live) ?? []
  const offline = channels?.filter((c) => !c.is_live) ?? []

  return (
    <div>
      {site?.setup_required && (
        <div className="banner accent">
          <h2>{t('home.welcomeTitle', { name: site.name })}</h2>
          <p>{t('home.welcomeSetup')}</p>
          <Link to="/register" className="btn primary">
            {t('home.welcomeSetupBtn')}
          </Link>
        </div>
      )}
      {site?.announcement && <div className="banner">{site.announcement}</div>}
      {q && <h1>{t('home.searchResults', { q })}</h1>}

      {cats.length > 0 && !q && (
        <div className="chips">
          <button className={'chip' + (category === '' ? ' active' : '')} onClick={() => setCategory('')}>
            {t('home.allCategories')}
          </button>
          {cats.map((c) => (
            <button key={c.name} className={'chip' + (category === c.name ? ' active' : '')} onClick={() => setCategory(c.name)}>
              {c.name}
              {c.live > 0 ? ` · ${c.live}` : ''}
            </button>
          ))}
        </div>
      )}

      <h2>{t('home.liveNow')}</h2>
      {channels === null ? (
        <p className="dim">{t('common.loading')}</p>
      ) : live.length === 0 ? (
        <p className="dim" style={{ marginBottom: 32 }}>
          {t('home.noLive')}
        </p>
      ) : (
        <div className="grid" data-testid="live-grid">
          {live.map((c) => (
            <ChannelCard key={c.id} ch={c} />
          ))}
        </div>
      )}

      {offline.length > 0 && (
        <>
          <h2>{t('home.channels')}</h2>
          <div className="grid">
            {offline.map((c) => (
              <ChannelCard key={c.id} ch={c} />
            ))}
          </div>
        </>
      )}
      {channels !== null && channels.length === 0 && !site?.setup_required && <p className="dim">{t('home.noChannels')}</p>}
    </div>
  )
}
