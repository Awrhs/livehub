import { useEffect, useRef, useState } from 'react'
import Hls from 'hls.js'
import mpegts from 'mpegts.js'
import { useI18n } from '../i18n'

type Mode = 'hls' | 'flv' | 'webrtc'
const MODE_KEY = 'livehub.playerMode'

function initialMode(): Mode {
  try {
    const v = localStorage.getItem(MODE_KEY)
    if (v === 'hls' || v === 'flv' || v === 'webrtc') return v
  } catch {
    // 忽略
  }
  return 'hls'
}

interface Props {
  hls: string
  flv: string
  abr?: string // 多清晰度主播放列表（开启转码时）
  webrtc?: string // WHEP 地址（站点开启 WebRTC 时）
}

// WHEP 播放：创建只收的 PeerConnection，POST offer 换取 answer（服务端为 ICE-lite，无需等待候选收集）
async function startWhep(url: string, video: HTMLVideoElement, onFail: () => void): Promise<() => void> {
  const pc = new RTCPeerConnection()
  pc.addTransceiver('video', { direction: 'recvonly' })
  pc.addTransceiver('audio', { direction: 'recvonly' })
  const ms = new MediaStream()
  pc.ontrack = (e) => {
    ms.addTrack(e.track)
    video.srcObject = ms
  }
  pc.onconnectionstatechange = () => {
    if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') onFail()
  }
  const offer = await pc.createOffer()
  await pc.setLocalDescription(offer)
  const resp = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/sdp' }, body: offer.sdp })
  if (!resp.ok) {
    pc.close()
    throw new Error('WHEP ' + resp.status)
  }
  await pc.setRemoteDescription({ type: 'answer', sdp: await resp.text() })
  return () => {
    pc.close()
    video.srcObject = null
  }
}

/**
 * 直播播放器：
 *  - 标准模式：HLS（hls.js；Safari 原生），稳定，延迟约 6-10 秒
 *  - 低延迟模式：HTTP-FLV（mpegts.js，基于 MSE），延迟约 1-3 秒
 * 浏览器禁止有声自动播放：先静音播放，提示用户点击开启声音。
 */
export default function Player({ hls, flv, abr, webrtc }: Props) {
  const { t } = useI18n()
  const videoRef = useRef<HTMLVideoElement>(null)
  const [modeState, setMode] = useState<Mode>(initialMode)
  // 站点未开启 WebRTC 时回落到 HLS
  const mode: Mode = modeState === 'webrtc' && !webrtc ? 'hls' : modeState
  const [muted, setMuted] = useState(true)
  const [error, setError] = useState('')
  const [levels, setLevels] = useState<{ index: number; label: string }[]>([])
  const [level, setLevel] = useState(-1)
  const hlsRef = useRef<Hls | null>(null)
  const [retry, setRetry] = useState(0)

  useEffect(() => {
    const video = videoRef.current
    if (!video) return
    setError('')
    setLevels([])
    let destroyed = false
    let retryTimer: number | undefined
    const scheduleRetry = () => {
      if (destroyed) return
      setError(t('player.error'))
      window.clearTimeout(retryTimer)
      retryTimer = window.setTimeout(() => setRetry((n) => n + 1), 3000)
    }
    const play = () => {
      video.muted = true
      video.play().catch(() => {
        // 自动播放被拒绝：等待用户交互
      })
    }
    let cleanup = () => {}

    // 浏览器不支持 H.264 解码（如部分 Linux 发行版的 Firefox 未安装解码器）：给出明确提示，不再无限重试
    const h264 =
      typeof MediaSource !== 'undefined' && MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E"')
    if (mode !== 'webrtc' && !h264 && !video.canPlayType('application/vnd.apple.mpegurl')) {
      setError(t('player.noH264'))
      return
    }
    if (mode === 'webrtc' && webrtc) {
      let stop: (() => void) | null = null
      startWhep(webrtc, video, scheduleRetry)
        .then((s) => {
          if (destroyed) s()
          else {
            stop = s
            play()
          }
        })
        .catch(scheduleRetry)
      cleanup = () => {
        stop?.()
      }
    } else if (mode === 'flv') {
      if (!mpegts.getFeatureList().mseLivePlayback) {
        setError(t('player.unsupported'))
        return
      }
      const player = mpegts.createPlayer(
        { type: 'flv', isLive: true, url: new URL(flv, window.location.href).toString() },
        { enableStashBuffer: false, liveBufferLatencyChasing: true, liveBufferLatencyMaxLatency: 3, liveBufferLatencyMinRemain: 0.5 },
      )
      player.attachMediaElement(video)
      player.on(mpegts.Events.ERROR, scheduleRetry)
      player.load()
      play()
      cleanup = () => {
        player.destroy()
      }
    } else {
      const src = abr || hls
      if (Hls.isSupported()) {
        const h = new Hls({ liveSyncDurationCount: 3, enableWorker: true, lowLatencyMode: false })
        hlsRef.current = h
        h.on(Hls.Events.MANIFEST_PARSED, (_e, data) => {
          if (data.levels.length > 1) {
            setLevels(
              data.levels
                .map((l, i) => ({ index: i, label: l.height ? l.height + 'p' : Math.round(l.bitrate / 1000) + 'k' }))
                .reverse(),
            )
          }
          play()
        })
        h.on(Hls.Events.ERROR, (_e, data) => {
          if (data.fatal) {
            h.destroy()
            scheduleRetry()
          }
        })
        h.loadSource(src)
        h.attachMedia(video)
        cleanup = () => {
          h.destroy()
          hlsRef.current = null
        }
      } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
        video.src = src
        video.addEventListener('loadedmetadata', play, { once: true })
        video.onerror = scheduleRetry
        cleanup = () => {
          video.removeAttribute('src')
          video.load()
        }
      } else {
        setError(t('player.unsupported'))
      }
    }
    return () => {
      destroyed = true
      window.clearTimeout(retryTimer)
      cleanup()
    }
    // t 变化（切换语言）不需要重建播放器
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [mode, hls, flv, abr, webrtc, retry])

  const changeMode = (m: Mode) => {
    setMode(m)
    try {
      localStorage.setItem(MODE_KEY, m)
    } catch {
      // 忽略
    }
  }

  const changeLevel = (v: number) => {
    setLevel(v)
    if (hlsRef.current) hlsRef.current.currentLevel = v
  }

  return (
    <div className="player">
      <video ref={videoRef} playsInline controls muted={muted} onVolumeChange={(e) => setMuted(e.currentTarget.muted)} data-testid="video" />
      {muted && !error && (
        <button
          className="unmute small"
          onClick={() => {
            const v = videoRef.current
            if (v) {
              v.muted = false
              void v.play().catch(() => {})
            }
          }}
        >
          🔇 {t('player.unmute')}
        </button>
      )}
      {error && (
        <div className="overlay" style={{ background: 'rgba(0,0,0,.6)' }}>
          {error}
        </div>
      )}
      <div className="controls">
        {levels.length > 0 && mode === 'hls' && (
          <select value={level} onChange={(e) => changeLevel(Number(e.target.value))} aria-label={t('player.quality')}>
            <option value={-1}>{t('player.auto')}</option>
            {levels.map((l) => (
              <option key={l.index} value={l.index}>
                {l.label}
              </option>
            ))}
          </select>
        )}
        <select value={mode} onChange={(e) => changeMode(e.target.value as Mode)} aria-label={t('player.mode')} data-testid="player-mode">
          <option value="hls">{t('player.modeHls')}</option>
          <option value="flv">{t('player.modeFlv')}</option>
          {webrtc && <option value="webrtc">{t('player.modeWebrtc')}</option>}
        </select>
      </div>
    </div>
  )
}
