import { Route, Routes, useLocation } from 'react-router-dom'
import Header from './components/Header'
import Home from './pages/Home'
import Login from './pages/Login'
import Register from './pages/Register'
import ChannelPage from './pages/Channel'
import Dashboard from './pages/Dashboard'
import Settings from './pages/Settings'
import Admin from './pages/Admin'
import NotFound from './pages/NotFound'
import { useSession } from './session'
import { useI18n } from './i18n'

export default function App() {
  const { loading } = useSession()
  const { t } = useI18n()
  const loc = useLocation()
  // 频道页（/<用户名>）使用全宽布局
  const reserved = ['/', '/login', '/register', '/dashboard', '/settings', '/admin', '/following', '/search']
  const isChannel = loc.pathname.split('/').length === 2 && !reserved.includes(loc.pathname)

  return (
    <>
      <Header />
      <main className={isChannel ? 'main full' : 'main'}>
        {loading ? (
          <p className="dim">{t('common.loading')}</p>
        ) : (
          <Routes>
            <Route path="/" element={<Home />} />
            <Route path="/search" element={<Home />} />
            <Route path="/login" element={<Login />} />
            <Route path="/register" element={<Register />} />
            <Route path="/dashboard" element={<Dashboard />} />
            <Route path="/settings" element={<Settings />} />
            <Route path="/admin" element={<Admin />} />
            <Route path="/:slug" element={<ChannelPage />} />
            <Route path="*" element={<NotFound />} />
          </Routes>
        )}
      </main>
    </>
  )
}
