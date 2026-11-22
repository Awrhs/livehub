import { useState, type FormEvent } from 'react'
import { Navigate } from 'react-router-dom'
import { patch, post, type Me } from '../api'
import { useI18n, type Lang } from '../i18n'
import { useSession } from '../session'

export default function Settings() {
  const { t, errText, lang, setLang } = useI18n()
  const { me, setMe } = useSession()
  const [displayName, setDisplayName] = useState(me?.display_name ?? '')
  const [bio, setBio] = useState(me?.bio ?? '')
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
