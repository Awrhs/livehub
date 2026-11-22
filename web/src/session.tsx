import { createContext, useCallback, useContext, useEffect, useState, type ReactNode } from 'react'
import { ApiError, get, post, type Me, type Site } from './api'

interface Session {
  site: Site | null
  me: Me | null
  loading: boolean
  refresh: () => Promise<void>
  setMe: (m: Me | null) => void
  logout: () => Promise<void>
}

const Ctx = createContext<Session | null>(null)

// 站点信息与当前登录用户
export function SessionProvider({ children }: { children: ReactNode }) {
  const [site, setSite] = useState<Site | null>(null)
  const [me, setMe] = useState<Me | null>(null)
  const [loading, setLoading] = useState(true)

  const refresh = useCallback(async () => {
    try {
      const [s, m] = await Promise.all([
        get<Site>('/site'),
        get<Me>('/me').catch((e) => {
          if (e instanceof ApiError && e.status === 401) return null
          throw e
        }),
      ])
      setSite(s)
      setMe(m)
      document.title = s.name
    } catch {
      // 网络错误时保留现状
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const logout = useCallback(async () => {
    try {
      await post('/auth/logout')
    } finally {
      setMe(null)
    }
  }, [])

  return <Ctx.Provider value={{ site, me, loading, refresh, setMe, logout }}>{children}</Ctx.Provider>
}

export function useSession(): Session {
  const v = useContext(Ctx)
  if (!v) throw new Error('useSession 必须在 SessionProvider 内使用')
  return v
}
