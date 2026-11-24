import { useCallback, useEffect, useState, type FormEvent } from 'react'
import { Link, Navigate } from 'react-router-dom'
import { del, get, patch, post, put, type User } from '../api'
import { useI18n } from '../i18n'
import type { MsgKey } from '../i18n/zh'
import { useSession } from '../session'

type Tab = 'overview' | 'approvals' | 'users' | 'reports' | 'settings' | 'invites' | 'audit'

interface Stats {
  users: number
  live_channels: number
  viewers: number
  pending_requests: number
  media_healthy: boolean
  media_enabled: boolean
  open_reports: number
  mail_enabled: boolean
  disk_low: boolean
  min_free_mb: number
  system: {
    cpu_percent: number
    cpus: number
    mem_total: number
    mem_used: number
    disk_total: number
    disk_free: number
    net_tx_bps: number
    net_rx_bps: number
    load1: number
  }
}

const gb = (b: number) => (b / 1024 ** 3).toFixed(1) + ' GB'
const mbps = (b: number) => (b / 1e6).toFixed(1) + ' Mbps'

// 资源占用条
function Meter({ label, value, max, text, warn }: { label: string; value: number; max: number; text: string; warn?: boolean }) {
  const pct = max > 0 ? Math.min(100, (value / max) * 100) : 0
  return (
    <div className="stat">
      <div className="k">{label}</div>
      <div style={{ fontSize: 18, fontWeight: 600, margin: '4px 0 8px' }}>{text}</div>
      <div className="meter">
        <div style={{ width: pct + '%', background: warn || pct > 90 ? 'var(--danger)' : pct > 75 ? 'var(--warn)' : 'var(--ok)' }} />
      </div>
    </div>
  )
}

export default function Admin() {
  const { t } = useI18n()
  const { me } = useSession()
  const [tab, setTab] = useState<Tab>('overview')
  if (!me) return <Navigate to="/login?next=/admin" replace />
  if (me.role !== 'admin') return <p className="error">{t('err.forbidden')}</p>
  const tabs: Tab[] = ['overview', 'approvals', 'users', 'reports', 'settings', 'invites', 'audit']
  return (
    <div>
      <h1>{t('admin.title')}</h1>
      <div className="tabs">
        {tabs.map((k) => (
          <button key={k} className={tab === k ? 'active' : ''} onClick={() => setTab(k)} data-testid={'tab-' + k}>
            {t(('admin.' + k) as MsgKey)}
          </button>
        ))}
      </div>
      {tab === 'overview' && <Overview />}
      {tab === 'approvals' && <Users pendingOnly />}
      {tab === 'users' && <Users />}
      {tab === 'reports' && <Reports />}
      {tab === 'settings' && <SiteSettings />}
      {tab === 'invites' && <Invites />}
      {tab === 'audit' && <Audit />}
    </div>
  )
}

function Overview() {
  const { t, errText } = useI18n()
  const [s, setS] = useState<Stats | null>(null)
  const [mailMsg, setMailMsg] = useState('')
  useEffect(() => {
    const load = () => get<Stats>('/admin/stats').then(setS).catch(() => {})
    load()
    const id = window.setInterval(load, 10000)
    return () => window.clearInterval(id)
  }, [])
  if (!s) return <p className="dim">{t('common.loading')}</p>
  const sys = s.system
  return (
    <>
    {s.disk_low && (
      <div className="banner warn" data-testid="disk-low">
        {t('admin.diskLow', { free: gb(sys.disk_free), min: s.min_free_mb })}
      </div>
    )}
    <h3>{t('admin.system')}</h3>
    <div className="stats">
      <Meter label={t('admin.cpu')} value={sys.cpu_percent} max={100} text={`${sys.cpu_percent.toFixed(0)}% · ${sys.cpus} ${t('admin.cores')}`} />
      <Meter label={t('admin.memory')} value={sys.mem_used} max={sys.mem_total} text={`${gb(sys.mem_used)} / ${gb(sys.mem_total)}`} />
      <Meter
        label={t('admin.disk')}
        value={sys.disk_total - sys.disk_free}
        max={sys.disk_total}
        warn={s.disk_low}
        text={`${t('admin.diskFree')} ${gb(sys.disk_free)} / ${gb(sys.disk_total)}`}
      />
      <div className="stat">
        <div className="k">{t('admin.network')}</div>
        <div style={{ fontSize: 18, fontWeight: 600, marginTop: 4 }}>↑ {mbps(sys.net_tx_bps)}</div>
        <div className="dim">↓ {mbps(sys.net_rx_bps)}</div>
      </div>
    </div>
    <h3>{t('admin.overview')}</h3>
    <div className="stats">
      <div className="stat">
        <div className="v">{s.users}</div>
        <div className="k">{t('admin.statUsers')}</div>
      </div>
      <div className="stat">
        <div className="v">{s.live_channels}</div>
        <div className="k">{t('admin.statLive')}</div>
      </div>
      <div className="stat">
        <div className="v">{s.viewers}</div>
        <div className="k">{t('admin.statViewers')}</div>
      </div>
      <div className="stat">
        <div className="v">{s.pending_requests}</div>
        <div className="k">{t('admin.statPending')}</div>
      </div>
      <div className="stat">
        <div className="v">{s.open_reports}</div>
        <div className="k">{t('admin.statReports')}</div>
      </div>
      <div className="stat">
        <div className="v" style={{ fontSize: 18, paddingTop: 8 }}>
          {!s.media_enabled ? (
            <span className="badge">{t('admin.mediaDisabled')}</span>
          ) : s.media_healthy ? (
            <span className="badge ok">{t('admin.mediaOk')}</span>
          ) : (
            <span className="badge danger">{t('admin.mediaDown')}</span>
          )}
        </div>
        <div className="k">{t('admin.mediaStatus')}</div>
      </div>
      <div className="stat">
        <div className="v" style={{ fontSize: 18, paddingTop: 8 }}>
          {s.mail_enabled ? (
            <button
              className="small"
              onClick={async () => {
                try {
                  await post('/admin/mail/test')
                  setMailMsg(t('admin.mailTestOk'))
                } catch (e) {
                  setMailMsg(errText(e))
                }
              }}
            >
              {t('admin.mailTest')}
            </button>
          ) : (
            <span className="badge">{t('admin.mediaDisabled')}</span>
          )}
        </div>
        <div className="k">
          {t('admin.mailStatus')} {mailMsg && <span className="dim">· {mailMsg}</span>}
        </div>
      </div>
    </div>
    </>
  )
}

function statusBadge(s: User['stream_status'], t: (k: MsgKey) => string) {
  const cls = s === 'approved' ? 'ok' : s === 'pending' ? 'warn' : s === 'rejected' ? 'danger' : ''
  return <span className={'badge ' + cls}>{t(('status.' + s) as MsgKey)}</span>
}

function Users({ pendingOnly = false }: { pendingOnly?: boolean }) {
  const { t, errText, ago } = useI18n()
  const { me } = useSession()
  const [users, setUsers] = useState<User[] | null>(null)
  const [total, setTotal] = useState(0)
  const [q, setQ] = useState('')
  const [offset, setOffset] = useState(0)
  const [err, setErr] = useState('')
  const limit = 50

  const load = useCallback(async () => {
    const qs = new URLSearchParams({ limit: String(limit), offset: String(offset) })
    if (pendingOnly) qs.set('stream_status', 'pending')
    if (q) qs.set('q', q)
    try {
      const r = await get<{ users: User[]; total: number }>('/admin/users?' + qs.toString())
      setUsers(r.users)
      setTotal(r.total)
    } catch (e) {
      setErr(errText(e))
    }
  }, [pendingOnly, q, offset, errText])

  useEffect(() => {
    void load()
  }, [load])

  const update = async (u: User, body: Record<string, unknown>) => {
    setErr('')
    try {
      await patch('/admin/users/' + u.id, body)
      await load()
    } catch (e) {
      setErr(errText(e))
    }
  }

  const resetPassword = async (u: User) => {
    if (!window.confirm(t('admin.resetPwConfirm', { name: u.username }))) return
    try {
      const r = await post<{ password: string }>('/admin/users/' + u.id + '/password')
      window.prompt(t('admin.resetPwDone', { name: u.username }), r.password)
    } catch (e) {
      setErr(errText(e))
    }
  }

  const ban = (u: User) => {
    const reason = window.prompt(t('admin.banReason'), '')
    if (reason === null) return
    void update(u, { banned: true, ban_reason: reason })
  }

  return (
    <div>
      {!pendingOnly && (
        <form
          className="row"
          style={{ marginBottom: 16, maxWidth: 480 }}
          onSubmit={(e: FormEvent) => {
            e.preventDefault()
            setOffset(0)
            void load()
          }}
        >
          <input value={q} onChange={(e) => setQ(e.target.value)} placeholder={t('admin.searchUsers')} />
          <button type="submit">{t('common.search')}</button>
        </form>
      )}
      {err && <div className="error">{err}</div>}
      {users === null ? (
        <p className="dim">{t('common.loading')}</p>
      ) : users.length === 0 ? (
        <p className="dim">{pendingOnly ? t('admin.noPending') : t('common.none')}</p>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>{t('auth.username')}</th>
                {pendingOnly ? <th>{t('dash.requestNote')}</th> : <th>{t('admin.role')}</th>}
                <th>{t('admin.streamStatus')}</th>
                <th>{t('admin.joined')}</th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              {users.map((u) => (
                <tr key={u.id} data-testid={'user-row-' + u.username}>
                  <td>
                    <Link to={'/' + u.username}>{u.display_name}</Link> <span className="dim">@{u.username}</span>
                    {u.banned && (
                      <>
                        {' '}
                        <span className="badge danger" title={u.ban_reason}>
                          {t('admin.banned')}
                        </span>
                      </>
                    )}
                  </td>
                  {pendingOnly ? (
                    <td style={{ maxWidth: 360, whiteSpace: 'pre-wrap' }}>{u.stream_request_note || <span className="dim">—</span>}</td>
                  ) : (
                    <td>
                      <select value={u.role} disabled={u.id === me?.id} onChange={(e) => void update(u, { role: e.target.value })}>
                        <option value="user">{t('admin.roleUser')}</option>
                        <option value="moderator">{t('admin.roleModerator')}</option>
                        <option value="admin">{t('admin.roleAdmin')}</option>
                      </select>
                    </td>
                  )}
                  <td>{statusBadge(u.stream_status, t)}</td>
                  <td className="dim">{ago(u.created_at)}</td>
                  <td>
                    <div className="row">
                      {u.stream_status !== 'approved' && (
                        <button className="small primary" onClick={() => void update(u, { stream_status: 'approved' })} data-testid="approve">
                          {t('admin.approve')}
                        </button>
                      )}
                      {u.stream_status === 'pending' && (
                        <button className="small" onClick={() => void update(u, { stream_status: 'rejected' })}>
                          {t('admin.reject')}
                        </button>
                      )}
                      {!pendingOnly && u.stream_status === 'approved' && (
                        <button className="small" onClick={() => void update(u, { stream_status: 'none' })}>
                          {t('admin.reject')}
                        </button>
                      )}
                      {!pendingOnly && (
                        <button className="small" onClick={() => void resetPassword(u)}>
                          {t('admin.resetPw')}
                        </button>
                      )}
                      {!pendingOnly && u.id !== me?.id &&
                        (u.banned ? (
                          <button className="small" onClick={() => void update(u, { banned: false, ban_reason: '' })}>
                            {t('admin.unban')}
                          </button>
                        ) : (
                          <button className="small danger" onClick={() => ban(u)}>
                            {t('admin.ban')}
                          </button>
                        ))}
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {total > limit && (
        <div className="row" style={{ marginTop: 16 }}>
          <button disabled={offset === 0} onClick={() => setOffset(Math.max(0, offset - limit))}>
            {t('common.prev')}
          </button>
          <span className="dim">
            {offset + 1}-{Math.min(offset + limit, total)} / {total}
          </span>
          <button disabled={offset + limit >= total} onClick={() => setOffset(offset + limit)}>
            {t('common.next')}
          </button>
        </div>
      )}
    </div>
  )
}

type SettingsMap = Record<string, string>

function SiteSettings() {
  const { t, errText } = useI18n()
  const { refresh } = useSession()
  const [s, setS] = useState<SettingsMap | null>(null)
  const [msg, setMsg] = useState('')
  const [err, setErr] = useState('')
  useEffect(() => {
    get<SettingsMap>('/admin/settings').then(setS).catch((e) => setErr(errText(e)))
  }, [errText])
  if (!s) return err ? <p className="error">{err}</p> : <p className="dim">{t('common.loading')}</p>
  const set = (k: string, v: string) => setS({ ...s, [k]: v })
  const bool = (k: string) => (
    <label className="check">
      <input type="checkbox" checked={s[k] === 'true'} onChange={(e) => set(k, e.target.checked ? 'true' : 'false')} />
      {t(
        ({
          stream_approval: 'admin.streamApproval',
          recording_enabled: 'admin.recordingEnabled',
          transcode_enabled: 'admin.transcodeEnabled',
        } as Record<string, MsgKey>)[k],
      )}
    </label>
  )
  const save = async (e: FormEvent) => {
    e.preventDefault()
    setErr('')
    try {
      setS(await put<SettingsMap>('/admin/settings', s))
      setMsg(t('common.saved'))
      window.setTimeout(() => setMsg(''), 2500)
      await refresh()
    } catch (e) {
      setErr(errText(e))
    }
  }
  return (
    <form className="card" onSubmit={save} style={{ maxWidth: 720 }}>
      <label className="field">
        <span>{t('admin.siteName')}</span>
        <input value={s.site_name} onChange={(e) => set('site_name', e.target.value)} maxLength={40} required />
      </label>
      <label className="field">
        <span>{t('admin.announcement')}</span>
        <textarea value={s.announcement} onChange={(e) => set('announcement', e.target.value)} maxLength={500} />
      </label>
      <label className="field">
        <span>{t('admin.registrationMode')}</span>
        <select value={s.registration_mode} onChange={(e) => set('registration_mode', e.target.value)} data-testid="registration-mode">
          <option value="open">{t('admin.regOpen')}</option>
          <option value="invite">{t('admin.regInvite')}</option>
          <option value="closed">{t('admin.regClosed')}</option>
        </select>
      </label>
      {bool('stream_approval')}
      {bool('recording_enabled')}
      {bool('transcode_enabled')}
      <label className="field">
        <span>{t('admin.slowMode')}</span>
        <input type="number" min={0} max={3600} value={s.chat_slow_mode_sec} onChange={(e) => set('chat_slow_mode_sec', e.target.value)} />
      </label>
      <label className="field">
        <span>{t('admin.vodRetention')}</span>
        <input type="number" min={0} max={3650} value={s.vod_retention_days} onChange={(e) => set('vod_retention_days', e.target.value)} />
      </label>
      <label className="field">
        <span>{t('admin.blockedWords')}</span>
        <textarea value={s.chat_blocked_words} onChange={(e) => set('chat_blocked_words', e.target.value)} />
      </label>
      {msg && <div className="success">{msg}</div>}
      {err && <div className="error">{err}</div>}
      <button className="primary" type="submit">
        {t('common.save')}
      </button>
    </form>
  )
}

interface Report {
  id: number
  reporter_name: string | null
  channel_slug: string
  channel_name: string
  channel_owner_id: number
  reason: string
  created_at: number
}

function Reports() {
  const { t, errText, ago } = useI18n()
  const [list, setList] = useState<Report[] | null>(null)
  const [err, setErr] = useState('')
  const load = useCallback(() => {
    get<{ reports: Report[] }>('/admin/reports')
      .then((r) => setList(r.reports))
      .catch((e) => setErr(errText(e)))
  }, [errText])
  useEffect(load, [load])
  const resolve = async (id: number, status: string) => {
    try {
      await post('/admin/reports/' + id, { status })
      load()
    } catch (e) {
      setErr(errText(e))
    }
  }
  const kick = async (slug: string) => {
    if (!window.confirm(t('admin.kickConfirm'))) return
    try {
      await post('/admin/channels/' + slug + '/kick')
    } catch (e) {
      setErr(errText(e))
    }
  }
  if (list === null) return err ? <p className="error">{err}</p> : <p className="dim">{t('common.loading')}</p>
  if (list.length === 0) return <p className="dim">{t('admin.noReports')}</p>
  return (
    <div className="table-wrap">
      {err && <div className="error">{err}</div>}
      <table>
        <thead>
          <tr>
            <th>{t('admin.target')}</th>
            <th>{t('admin.reason')}</th>
            <th>{t('admin.reporter')}</th>
            <th>{t('admin.time')}</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          {list.map((r) => (
            <tr key={r.id}>
              <td>
                <Link to={'/' + r.channel_slug}>{r.channel_name}</Link>
              </td>
              <td style={{ maxWidth: 360, whiteSpace: 'pre-wrap' }}>{r.reason}</td>
              <td>{r.reporter_name ?? '—'}</td>
              <td className="dim">{ago(r.created_at)}</td>
              <td>
                <div className="row">
                  <button className="small danger" onClick={() => void kick(r.channel_slug)}>
                    {t('admin.kick')}
                  </button>
                  <button className="small primary" onClick={() => void resolve(r.id, 'resolved')}>
                    {t('admin.reportResolve')}
                  </button>
                  <button className="small" onClick={() => void resolve(r.id, 'dismissed')}>
                    {t('admin.reportDismiss')}
                  </button>
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

interface Invite {
  code: string
  used_by: number | null
  used_by_name: string
  created_at: number
  expires_at: number | null
}

function Invites() {
  const { t, errText, ago } = useI18n()
  const [list, setList] = useState<Invite[]>([])
  const [hours, setHours] = useState(72)
  const [err, setErr] = useState('')
  const load = useCallback(() => {
    get<{ invites: Invite[] }>('/admin/invites')
      .then((r) => setList(r.invites))
      .catch((e) => setErr(errText(e)))
  }, [errText])
  useEffect(load, [load])
  const create = async (e: FormEvent) => {
    e.preventDefault()
    try {
      await post('/admin/invites', { expires_in_hours: hours })
      load()
    } catch (e) {
      setErr(errText(e))
    }
  }
  const remove = async (code: string) => {
    try {
      await del('/admin/invites/' + code)
      load()
    } catch (e) {
      setErr(errText(e))
    }
  }
  return (
    <div>
      <form className="row" onSubmit={create} style={{ marginBottom: 16, maxWidth: 480 }}>
        <label className="dim" style={{ whiteSpace: 'nowrap' }}>
          {t('admin.expiresHours')}
        </label>
        <input type="number" min={0} value={hours} onChange={(e) => setHours(Number(e.target.value))} style={{ width: 100 }} />
        <button className="primary" type="submit">
          {t('admin.createInvite')}
        </button>
      </form>
      {err && <div className="error">{err}</div>}
      <div className="table-wrap">
        <table>
          <tbody>
            {list.map((i) => (
              <tr key={i.code}>
                <td className="mono">{i.code}</td>
                <td>{i.used_by ? t('admin.inviteUsed', { name: i.used_by_name }) : <span className="badge ok">{t('admin.inviteUnused')}</span>}</td>
                <td className="dim">{ago(i.created_at)}</td>
                <td className="dim">{i.expires_at ? new Date(i.expires_at * 1000).toLocaleString() : '∞'}</td>
                <td>
                  {!i.used_by && (
                    <button className="small danger" onClick={() => void remove(i.code)}>
                      {t('common.delete')}
                    </button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}

interface AuditEntry {
  id: number
  actor_name: string
  action: string
  target: string
  detail: string
  created_at: number
}

function Audit() {
  const { t } = useI18n()
  const [list, setList] = useState<AuditEntry[]>([])
  useEffect(() => {
    get<{ entries: AuditEntry[] }>('/admin/audit').then((r) => setList(r.entries)).catch(() => {})
  }, [])
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th>{t('admin.time')}</th>
            <th>{t('admin.actor')}</th>
            <th>{t('admin.action')}</th>
            <th>{t('admin.target')}</th>
            <th>{t('admin.detail')}</th>
          </tr>
        </thead>
        <tbody>
          {list.map((e) => (
            <tr key={e.id}>
              <td className="dim">{new Date(e.created_at * 1000).toLocaleString()}</td>
              <td>{e.actor_name || 'system'}</td>
              <td className="mono">{e.action}</td>
              <td>{e.target}</td>
              <td className="dim">{e.detail}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}
