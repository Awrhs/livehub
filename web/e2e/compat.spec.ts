import { test, expect, type Page } from '@playwright/test'
import { spawn, type ChildProcess } from 'node:child_process'

// 各浏览器播放兼容性。每个浏览器项目使用独立的主播账号与频道。
const H = { 'X-Requested-With': 'livehub' }
let push: ChildProcess | null = null

test.afterEach(() => {
  push?.kill('SIGINT')
  push = null
})

async function playing(page: Page, label: string) {
  const video = page.getByTestId('video')
  await expect(video).toBeVisible()
  console.log(`[${label}] 开始验证播放`)
  const diag = async () =>
    video.evaluate((v: HTMLVideoElement) => ({
      t: v.currentTime, rs: v.readyState, ns: v.networkState, paused: v.paused,
      err: v.error ? `${v.error.code} ${v.error.message}` : null,
      buf: v.buffered.length ? [v.buffered.start(0), v.buffered.end(v.buffered.length - 1)] : null,
    }))
  const timer = setInterval(async () => console.log(`[${label}]`, JSON.stringify(await diag().catch(() => null))), 5000)
  try {
    await playingInner(page)
  } finally {
    clearInterval(timer)
  }
}

async function playingInner(page: Page) {
  const video = page.getByTestId('video')
  await expect
    .poll(async () => video.evaluate((v: HTMLVideoElement) => v.currentTime), { timeout: 60_000, intervals: [1000] })
    .toBeGreaterThan(2)
  const t1 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  await page.waitForTimeout(3000)
  const t2 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  expect(t2 - t1).toBeGreaterThan(1) // 持续播放而不是卡住
}

test('直播播放兼容性（OBS 风格推流，含 B 帧）', async ({ page, browserName }, info) => {
  // 首个注册账号为管理员：用它关闭开播审批，后续各浏览器的测试账号即可直接推流
  let a = await page.request.post('/api/v1/auth/register', { headers: H, data: { username: 'compatadmin', password: 'password123' } })
  if (!a.ok()) a = await page.request.post('/api/v1/auth/login', { headers: H, data: { username: 'compatadmin', password: 'password123' } })
  expect(a.ok()).toBeTruthy()
  if ((await a.json()).role === 'admin') {
    await page.request.put('/api/v1/admin/settings', { headers: H, data: { stream_approval: 'false' } })
  }
  await page.request.post('/api/v1/auth/logout', { headers: H })
  const user = 'compat' + info.project.name.replace(/\W/g, '')
  // 注册并生成推流码
  let r = await page.request.post('/api/v1/auth/register', { headers: H, data: { username: user, password: 'password123' } })
  if (!r.ok()) r = await page.request.post('/api/v1/auth/login', { headers: H, data: { username: user, password: 'password123' } })
  expect(r.ok()).toBeTruthy()
  const key = (await (await page.request.post('/api/v1/me/channel/key', { headers: H })).json()).new_key.publish_key
  push = spawn('ffmpeg', [
    '-hide_banner', '-loglevel', 'error', '-re',
    '-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=30',
    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
    '-t', '150',
    // OBS 默认 x264：High profile、B 帧、2 秒关键帧
    '-c:v', 'libx264', '-preset', 'veryfast', '-profile:v', 'high', '-bf', '2', '-g', '60', '-b:v', '2500k', '-pix_fmt', 'yuv420p',
    '-c:a', 'aac', '-b:a', '160k', '-f', 'flv', `rtmp://127.0.0.1:1935/live/${key}`,
  ], { stdio: 'inherit' })

  page.on('console', (m) => {
    if (m.type() === 'error' || m.type() === 'warning' || /error|Error/.test(m.text())) console.log('[browser]', m.type(), m.text().slice(0, 300))
  })
  page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)))
  await page.goto('/' + user)
  await expect(page.getByTestId('live-badge')).toBeVisible({ timeout: 30_000 })

  // 浏览器自身不支持 H.264（如 Playwright 自带的 Firefox 构建）：验证播放器给出明确提示后结束
  const h264 = await page.evaluate(() => typeof MediaSource !== 'undefined' && MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E"'))
  if (!h264) {
    await expect(page.getByText('无法解码 H.264')).toBeVisible()
    test.info().annotations.push({ type: 'limitation', description: `${browserName} 构建不含 H.264 解码器，跳过播放验证` })
    return
  }

  // 标准模式（HLS）
  await page.getByTestId('player-mode').selectOption('hls')
  await playing(page, browserName + ' HLS')

  // 低延迟模式（HTTP-FLV）
  await page.getByTestId('player-mode').selectOption('flv')
  await playing(page, browserName + ' FLV')

  // 超低延迟（WebRTC）：Chrome 与 Firefox
  if (browserName !== 'webkit') {
    await page.getByTestId('player-mode').selectOption('webrtc')
    // 浏览器的 WebRTC 若无法使用 H.264（Playwright 的 Firefox 不带 OpenH264 插件），
    // 服务器拒绝协商，播放器应给出明确提示；否则应正常播放
    const noH264 = page.getByText('WebRTC 不支持 H.264')
    const outcome = await Promise.race([
      noH264.waitFor({ timeout: 20_000 }).then(() => 'no-h264').catch(() => 'timeout'),
      page.waitForFunction(() => (document.querySelector('[data-testid=video]') as HTMLVideoElement | null)!.currentTime > 2,
        null, { timeout: 20_000 }).then(() => 'playing').catch(() => 'timeout'),
    ])
    if (outcome === 'no-h264') {
      test.info().annotations.push({ type: 'limitation', description: `${browserName} 的 WebRTC 不含 H.264，已显示明确提示` })
      console.log(`[${browserName} WebRTC] 浏览器不支持 H.264，播放器已给出提示`)
    } else {
      await playing(page, browserName + ' WebRTC')
    }
  }
})
