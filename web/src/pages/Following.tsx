import { useEffect, useState } from 'react'
import { Navigate } from 'react-router-dom'
import { get, type Channel } from '../api'
import ChannelCard from '../components/ChannelCard'
import { useI18n } from '../i18n'
import { useSession } from '../session'

export default function Following() {
  const { t } = useI18n()
  const { me } = useSession()
  const [list, setList] = useState<Channel[] | null>(null)
  useEffect(() => {
    if (!me) return
    const load = () =>
      get<{ channels: Channel[] }>('/me/following')
        .then((r) => setList(r.channels))
        .catch(() => setList([]))
    void load()
    const id = window.setInterval(load, 20000)
    return () => window.clearInterval(id)
  }, [me])
  if (!me) return <Navigate to="/login?next=/following" replace />
  return (
    <div>
      <h1>{t('follow.title')}</h1>
      {list === null ? (
        <p className="dim">{t('common.loading')}</p>
      ) : list.length === 0 ? (
        <p className="dim">{t('follow.empty')}</p>
      ) : (
        <div className="grid">
          {list.map((c) => (
            <ChannelCard key={c.id} ch={c} />
          ))}
        </div>
      )}
    </div>
  )
}
