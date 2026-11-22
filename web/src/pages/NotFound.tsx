import { Link } from 'react-router-dom'
import { useI18n } from '../i18n'

export default function NotFound({ text }: { text?: string }) {
  const { t } = useI18n()
  return (
    <div className="center" style={{ padding: '80px 20px' }}>
      <h1>{text || t('notfound.title')}</h1>
      <Link to="/" className="btn">
        {t('notfound.home')}
      </Link>
    </div>
  )
}
