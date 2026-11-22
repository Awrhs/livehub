// LiveHub REST API 客户端与类型定义

export interface User {
  id: number
  username: string
  display_name: string
  role: 'admin' | 'moderator' | 'user'
  stream_status: 'none' | 'pending' | 'approved' | 'rejected'
  stream_request_note: string
  bio: string
  banned: boolean
  ban_reason: string
  created_at: number
  last_login_at: number | null
}

export interface Channel {
  id: number
  user_id: number
  slug: string
  title: string
  category: string
  description: string
  is_live: boolean
  live_started_at: number | null
  last_live_at: number | null
  record_enabled: boolean
  created_at: number
  display_name: string
  viewers?: number
  playback?: { hls: string; flv: string; abr?: string; webrtc?: string; edge?: string }
  thumbnail?: string
}

export interface Ingest {
  rtmp: string
  srt?: string
}

export interface Me extends User {
  can_stream: boolean
  channel: Channel & { stream_key: string; publish_key: string; srt_url?: string }
  ingest: Ingest
}

export interface Site {
  name: string
  announcement: string
  registration_mode: 'open' | 'invite' | 'closed'
  stream_approval: boolean
  setup_required: boolean
  version: string
  ingest: Ingest
  media_enabled: boolean
}

export interface ChatMessage {
  id: number
  user: { id: number; username: string; display_name: string; role: string }
  body: string
  ts: number
}

export interface VOD {
  id: number
  channel_id: number
  channel_slug: string
  display_name: string
  title: string
  category: string
  duration_sec: number
  size_bytes: number
  views: number
  created_at: number
  url: string
  thumbnail?: string
}

export interface Notification {
  id: number
  kind: 'live' | 'stream_approved' | 'stream_rejected' | string
  channel_slug: string | null
  channel_name: string | null
  body: string
  created_at: number
  read_at: number | null
}

export class ApiError extends Error {
  status: number
  code: string
  constructor(status: number, code: string, message: string) {
    super(message)
    this.status = status
    this.code = code
  }
}

export async function api<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { 'X-Requested-With': 'livehub' }
  if (body !== undefined) headers['Content-Type'] = 'application/json'
  let resp: Response
  try {
    resp = await fetch('/api/v1' + path, {
      method,
      headers,
      credentials: 'same-origin',
      body: body === undefined ? undefined : JSON.stringify(body),
    })
  } catch {
    throw new ApiError(0, 'network', '网络错误')
  }
  const text = await resp.text()
  let data: unknown = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      data = null
    }
  }
  if (!resp.ok) {
    const e = (data as { error?: { code?: string; message?: string } } | null)?.error
    throw new ApiError(resp.status, e?.code || 'http_' + resp.status, e?.message || resp.statusText)
  }
  return data as T
}

export const get = <T,>(path: string) => api<T>('GET', path)
export const post = <T,>(path: string, body: unknown = {}) => api<T>('POST', path, body)
export const patch = <T,>(path: string, body: unknown) => api<T>('PATCH', path, body)
export const put = <T,>(path: string, body: unknown) => api<T>('PUT', path, body)
export const del = <T,>(path: string) => api<T>('DELETE', path)
