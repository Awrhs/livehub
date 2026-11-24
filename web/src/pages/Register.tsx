import { useState, type FormEvent } from 'react'
import { Link, Navigate, useNavigate } from 'react-router-dom'
import { post, type Me } from '../api'
import { useI18n } from '../i18n'
import { useSession } from '../session'

export default function Register() {
  const { t, errText } = useI18n()
  const { me, setMe, site, refresh } = useSession()
  const nav = useNavigate()
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [displayName, setDisplayName] = useState('')
  const [invite, setInvite] = useState('')
  const [email, setEmail] = useState('')
  const [err, setErr] = useState('')
  const [busy, setBusy] = useState(false)

  if (me) return <Navigate to="/" replace />
  const setup = site?.setup_required
  if (site && !setup && site.registration_mode === 'closed') {
    return (
      <div className="auth-box card">
        <h1>{t('auth.registerTitle')}</h1>
        <p>{t('auth.registrationClosed')}</p>
      </div>
    )
  }
  const needInvite = !setup && site?.registration_mode === 'invite'

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true)
    setErr('')
    try {
      const m = await post<Me>('/auth/register', { username, password, display_name: displayName, invite, email })
      setMe(m)
      await refresh()
      nav(m.role === 'admin' ? '/admin' : '/dashboard', { replace: true })
    } catch (e) {
      setErr(errText(e))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="auth-box card">
      <h1>{t('auth.registerTitle')}</h1>
      {setup && <div className="banner accent">{t('auth.firstAdmin')}</div>}
      <form onSubmit={submit}>
        <label className="field">
          <span>{t('auth.username')}</span>
          <input name="username" autoComplete="username" value={username} onChange={(e) => setUsername(e.target.value)} required autoFocus pattern="[A-Za-z][A-Za-z0-9_]{2,23}" />
          <div className="hint">{t('auth.usernameHint')}</div>
        </label>
        <label className="field">
          <span>{t('auth.displayName')}</span>
          <input name="display_name" value={displayName} onChange={(e) => setDisplayName(e.target.value)} maxLength={32} />
        </label>
        <label className="field">
          <span>{t('auth.password')}</span>
          <input name="password" type="password" autoComplete="new-password" value={password} onChange={(e) => setPassword(e.target.value)} required minLength={8} maxLength={72} />
          <div className="hint">{t('auth.passwordHint')}</div>
        </label>
        {site?.mail_enabled && (
          <label className="field">
            <span>{t('email.optional')}</span>
            <input name="email" type="email" autoComplete="email" value={email} onChange={(e) => setEmail(e.target.value)} />
            <div className="hint">{t('email.hint')}</div>
          </label>
        )}
        {needInvite && (
          <label className="field">
            <span>{t('auth.invite')}</span>
            <input name="invite" value={invite} onChange={(e) => setInvite(e.target.value)} required />
          </label>
        )}
        {err && <div className="error">{err}</div>}
        <button className="primary" type="submit" disabled={busy} style={{ width: '100%' }}>
          {t('auth.registerTitle')}
        </button>
      </form>
      <p className="dim center" style={{ marginTop: 16 }}>
        {t('auth.haveAccount')} <Link to="/login">{t('nav.login')}</Link>
      </p>
    </div>
  )
}
