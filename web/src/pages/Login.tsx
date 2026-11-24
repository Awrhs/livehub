import { useState, type FormEvent } from 'react'
import { Link, Navigate, useNavigate, useSearchParams } from 'react-router-dom'
import { post, type Me } from '../api'
import { useI18n } from '../i18n'
import { useSession } from '../session'

// 只允许站内相对路径作为登录后跳转目标
export function safeNext(next: string | null): string {
  return next && next.startsWith('/') && !next.startsWith('//') ? next : '/'
}

export default function Login() {
  const { t, errText } = useI18n()
  const { me, setMe, site } = useSession()
  const nav = useNavigate()
  const [params] = useSearchParams()
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [err, setErr] = useState('')
  const [busy, setBusy] = useState(false)

  if (me) return <Navigate to={safeNext(params.get('next'))} replace />

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true)
    setErr('')
    try {
      const m = await post<Me>('/auth/login', { username, password })
      setMe(m)
      nav(safeNext(params.get('next')), { replace: true })
    } catch (e) {
      setErr(errText(e))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="auth-box card">
      <h1>{t('auth.loginTitle')}</h1>
      <form onSubmit={submit}>
        <label className="field">
          <span>{t('auth.username')}</span>
          <input name="username" autoComplete="username" value={username} onChange={(e) => setUsername(e.target.value)} required autoFocus />
        </label>
        <label className="field">
          <span>{t('auth.password')}</span>
          <input name="password" type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} required />
        </label>
        {params.get('reset') && <div className="success">{t('reset.done')}</div>}
        {err && <div className="error">{err}</div>}
        <button className="primary" type="submit" disabled={busy} style={{ width: '100%' }}>
          {t('auth.loginTitle')}
        </button>
      </form>
      <p className="center" style={{ marginTop: 12 }}>
        <Link to="/forgot" className="dim">
          {t('forgot.link')}
        </Link>
      </p>
      {site?.registration_mode !== 'closed' && (
        <p className="dim center" style={{ marginTop: 16 }}>
          {t('auth.noAccount')} <Link to="/register">{t('nav.register')}</Link>
        </p>
      )}
    </div>
  )
}
