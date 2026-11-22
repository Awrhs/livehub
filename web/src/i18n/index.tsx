import { createContext, useCallback, useContext, useMemo, useState, type ReactNode } from 'react'
import zh, { type MsgKey } from './zh'
import en from './en'
import { ApiError } from '../api'

export type Lang = 'zh' | 'en'
const dicts = { zh, en }
const STORAGE_KEY = 'livehub.lang'

function initialLang(): Lang {
  try {
    const v = localStorage.getItem(STORAGE_KEY)
    if (v === 'zh' || v === 'en') return v
  } catch {
    // 隐私模式等情况下 localStorage 不可用
  }
  return 'zh' // 默认中文
}

type TFunc = (key: MsgKey, vars?: Record<string, string | number>) => string

interface I18n {
  lang: Lang
  setLang: (l: Lang) => void
  t: TFunc
  errText: (e: unknown) => string
  ago: (unixSec: number) => string
  duration: (sec: number) => string
}

const Ctx = createContext<I18n | null>(null)

export function I18nProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(initialLang)
  const setLang = useCallback((l: Lang) => {
    setLangState(l)
    try {
      localStorage.setItem(STORAGE_KEY, l)
    } catch {
      // 忽略
    }
    document.documentElement.lang = l === 'zh' ? 'zh-CN' : 'en'
  }, [])
  const value = useMemo<I18n>(() => {
    const dict = dicts[lang]
    const t: TFunc = (key, vars) => {
      let s: string = dict[key] ?? zh[key] ?? key
      if (vars) for (const [k, v] of Object.entries(vars)) s = s.split('{' + k + '}').join(String(v))
      return s
    }
    const errText = (e: unknown) => {
      if (e instanceof ApiError) {
        const k = ('err.' + e.code) as MsgKey
        if (k in dict) return t(k)
        return e.message
      }
      return String(e)
    }
    const ago = (sec: number) => {
      const d = Math.max(0, Date.now() / 1000 - sec)
      if (d < 60) return t('time.justNow')
      if (d < 3600) return t('time.minutesAgo', { n: Math.floor(d / 60) })
      if (d < 86400) return t('time.hoursAgo', { n: Math.floor(d / 3600) })
      return t('time.daysAgo', { n: Math.floor(d / 86400) })
    }
    const duration = (sec: number) => {
      sec = Math.max(0, Math.floor(sec))
      const h = Math.floor(sec / 3600)
      const m = Math.floor((sec % 3600) / 60)
      const s = sec % 60
      const pad = (n: number) => String(n).padStart(2, '0')
      return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`
    }
    return { lang, setLang, t, errText, ago, duration }
  }, [lang, setLang])
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>
}

export function useI18n(): I18n {
  const v = useContext(Ctx)
  if (!v) throw new Error('useI18n 必须在 I18nProvider 内使用')
  return v
}
