import { useState, type FormEvent } from 'react'
import { Navigate } from 'react-router-dom'
import { ApiError, del, patch, post, type Me } from '../api'
import Avatar from '../components/Avatar'
import { useI18n, type Lang } from '../i18n'
import { useSession } from '../session'

export default function Settings() {
  const { t, errText, lang, setLang } = useI18n()
  const { me, setMe } = useSession()
  const [displayName, setDisplayName] = useState(me?.display_name ?? '')
  const [bio, setBio] = useState(me?.bio ?? '')
  const { site } = useSession()
  const [email, setEmail] = useState(me?.email ?? '')
  const [oldPw, setOldPw] = useState('')
  const [newPw, setNewPw] = useState('')
  const [msg, setMsg] = useState('')
  const [err, setErr] = useState('')

  if (!me) return <Navigate to="/login?next=/settings" replace />

  const saveProfile = async (e: FormEvent) => {
    e.preventDefault()
    setErr('')
    try {
      setMe(await patch<Me>('/me', { display_name: displayName, bio }))
      setMsg(t('common.saved'))
    } catch (e) {
      setErr(errText(e))
    }
  }

  const changePw = async (e: FormEvent) => {
    e.preventDefault()
    setErr('')
    try {
      await post('/me/password', { old_password: oldPw, new_password: newPw })
      setOldPw('')
      setNewPw('')
      setMsg(t('settings.passwordChanged'))
    } catch (e) {
      setErr(errText(e))
    }
  }

  return (
    <div style={{ maxWidth: 640 }}>
      <h1>{t('settings.title')}</h1>
      {msg && <div className="success">{msg}</div>}
      {err && <div className="error">{err}</div>}
      <form className="card" onSubmit={saveProfile}>
        <h2>{t('settings.profile')}</h2>
        <div className="row" style={{ marginBottom: 16 }}>
          <Avatar name={me.display_name} url={me.avatar_url} size="lg" />
          <label className="btn small" style={{ cursor: 'pointer' }}>
            {t('settings.uploadAvatar')}
            <input
              type="file"
              accept="image/png,image/jpeg,image/gif,image/webp"
              style={{ display: 'none' }}
              data-testid="avatar-input"
              onChange={async (e) => {
                const f = e.target.files?.[0]
                e.target.value = ''
                if (!f) return
                setErr('')
                try {
                  const resp = await fetch('/api/v1/me/avatar', {
                    method: 'POST',
                    headers: { 'X-Requested-With': 'livehub', 'Content-Type': f.type || 'application/octet-stream' },
                    body: f,
                  })
                  const data = await resp.json()
                  if (!resp.ok) throw new ApiError(resp.status, data?.error?.code || 'http', data?.error?.message || '')
                  setMe(data as Me)
                } catch (e) {
                  setErr(errText(e))
                }
              }}
            />
          </label>
          {me.avatar_url && (
            <button type="button" className="small" onClick={async () => setMe(await del<Me>('/me/avatar'))}>
              {t('settings.removeAvatar')}
            </button>
          )}
        </div>
        <label className="field">
          <span>{t('auth.displayName')}</span>
          <input value={displayName} onChange={(e) => setDisplayName(e.target.value)} maxLength={32} required />
        </label>
        <label className="field">
          <span>{t('settings.bio')}</span>
          <textarea value={bio} onChange={(e) => setBio(e.target.value)} maxLength={500} />
        </label>
        <button className="primary" type="submit">
          {t('common.save')}
        </button>
      </form>
      {site?.mail_enabled && (
        <form
          className="card"
          onSubmit={async (e) => {
            e.preventDefault()
            setErr('')
            try {
              setMe(await post<Me>('/me/email', { email }))
              setMsg(email ? t('email.sent') : t('common.saved'))
            } catch (e) {
              setErr(errText(e))
            }
          }}
        >
          <h2>{t('email.title')}</h2>
          <label className="field">
            <span>
              {t('email.address')}{' '}
              {me.email &&
                (me.email_verified ? (
                  <span className="badge ok">{t('email.verified')}</span>
                ) : (
                  <span className="badge warn">{t('email.unverified')}</span>
                ))}
            </span>
            <input type="email" value={email} onChange={(e) => setEmail(e.target.value)} data-testid="email-input" />
            <div className="hint">{t('email.hint')}</div>
          </label>
          <div className="row">
            <button className="primary" type="submit">
              {t('common.save')}
            </button>
            {me.email && !me.email_verified && (
              <button
                type="button"
                onClick={async () => {
                  setErr('')
                  try {
                    await post('/me/email/resend')
                    setMsg(t('email.sent'))
                  } catch (e) {
                    setErr(errText(e))
                  }
                }}
              >
                {t('email.resend')}
              </button>
            )}
          </div>
        </form>
      )}
      <form className="card" onSubmit={changePw}>
        <h2>{t('settings.password')}</h2>
        <label className="field">
          <span>{t('settings.oldPassword')}</span>
          <input type="password" autoComplete="current-password" value={oldPw} onChange={(e) => setOldPw(e.target.value)} required />
        </label>
        <label className="field">
          <span>{t('settings.newPassword')}</span>
          <input type="password" autoComplete="new-password" value={newPw} onChange={(e) => setNewPw(e.target.value)} required minLength={8} maxLength={72} />
        </label>
        <button className="primary" type="submit">
          {t('common.save')}
        </button>
      </form>
      <div className="card">
        <h2>{t('settings.language')}</h2>
        <select value={lang} onChange={(e) => setLang(e.target.value as Lang)} style={{ width: 'auto' }}>
          <option value="zh">简体中文</option>
          <option value="en">English</option>
        </select>
      </div>
    </div>
  )
}
