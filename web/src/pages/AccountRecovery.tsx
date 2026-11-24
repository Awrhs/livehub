import { useEffect, useRef, useState, type FormEvent } from 'react'
import { Link, useNavigate, useSearchParams } from 'react-router-dom'
import { post } from '../api'
import { useI18n } from '../i18n'
import { useSession } from '../session'

// 忘记密码：输入用户名或邮箱，发送重置邮件
export function Forgot() {
  const { t, errText } = useI18n()
  const { site } = useSession()
  const [login, setLogin] = useState('')
  const [done, setDone] = useState(false)
  const [err, setErr] = useState('')
  if (site && !site.mail_enabled) {
    return (
      <div className="auth-box card">
        <h1>{t('forgot.title')}</h1>
        <p>{t('forgot.noMail')}</p>
      </div>
    )
  }
  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setErr('')
    try {
      await post('/auth/forgot', { login })
      setDone(true)
    } catch (e) {
      setErr(errText(e))
    }
  }
  return (
    <div className="auth-box card">
      <h1>{t('forgot.title')}</h1>
      {done ? (
        <p className="success" data-testid="forgot-done">{t('forgot.sent')}</p>
      ) : (
        <form onSubmit={submit}>
          <label className="field">
            <span>{t('forgot.login')}</span>
            <input name="login" value={login} onChange={(e) => setLogin(e.target.value)} required autoFocus />
          </label>
          {err && <div className="error">{err}</div>}
          <button className="primary" type="submit" style={{ width: '100%' }}>
            {t('forgot.submit')}
          </button>
        </form>
      )}
      <p className="dim center" style={{ marginTop: 16 }}>
        <Link to="/login">{t('nav.login')}</Link>
      </p>
    </div>
  )
}

// 通过邮件链接设置新密码
export function ResetPassword() {
  const { t, errText } = useI18n()
  const [params] = useSearchParams()
  const nav = useNavigate()
  const [pw, setPw] = useState('')
  const [pw2, setPw2] = useState('')
  const [err, setErr] = useState('')
  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (pw !== pw2) {
      setErr(t('reset.mismatch'))
      return
    }
    try {
      await post('/auth/reset', { token: params.get('token') || '', password: pw })
      nav('/login?reset=1', { replace: true })
    } catch (e) {
      setErr(errText(e))
    }
  }
  return (
    <div className="auth-box card">
      <h1>{t('reset.title')}</h1>
      <form onSubmit={submit}>
        <label className="field">
          <span>{t('settings.newPassword')}</span>
          <input name="password" type="password" autoComplete="new-password" value={pw} onChange={(e) => setPw(e.target.value)} required minLength={8} maxLength={72} />
        </label>
        <label className="field">
          <span>{t('reset.confirm')}</span>
          <input name="password2" type="password" autoComplete="new-password" value={pw2} onChange={(e) => setPw2(e.target.value)} required minLength={8} maxLength={72} />
        </label>
        {err && <div className="error">{err}</div>}
        <button className="primary" type="submit" style={{ width: '100%' }}>
          {t('common.save')}
        </button>
      </form>
    </div>
  )
}

// 邮箱验证链接落地页
export function VerifyEmail() {
  const { t, errText } = useI18n()
  const [params] = useSearchParams()
  const { refresh } = useSession()
  const [state, setState] = useState<'pending' | 'ok' | 'error'>('pending')
  const [err, setErr] = useState('')
  const once = useRef(false)
  useEffect(() => {
    if (once.current) return // StrictMode 下 effect 会执行两次，而令牌只能使用一次
    once.current = true
    post('/auth/verify-email', { token: params.get('token') || '' })
      .then(() => {
        setState('ok')
        void refresh()
      })
      .catch((e) => {
        setErr(errText(e))
        setState('error')
      })
  }, [params, errText, refresh])
  return (
    <div className="auth-box card">
      <h1>{t('verify.title')}</h1>
      {state === 'pending' && <p className="dim">{t('common.loading')}</p>}
      {state === 'ok' && <p className="success" data-testid="verify-ok">{t('verify.ok')}</p>}
      {state === 'error' && <p className="error">{err}</p>}
      <Link to="/" className="btn">
        {t('notfound.home')}
      </Link>
    </div>
  )
}
