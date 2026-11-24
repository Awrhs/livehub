import { test, expect, type Page } from '@playwright/test'
import { spawn, type ChildProcess } from 'node:child_process'

// 各浏览器播放兼容性。每个浏览器项目使用独立的主播账号与频道。
const H = { 'X-Requested-With': 'livehub' }
let push: ChildProcess | null = null

test.afterEach(() => {
  push?.kill('SIGINT')
  push = null
})

async function playing(page: Page) {
  const video = page.getByTestId('video')
  await expect(video).toBeVisible()
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

  await page.goto('/' + user)
  await expect(page.getByTestId('live-badge')).toBeVisible({ timeout: 30_000 })

  // 标准模式（HLS）
  await page.getByTestId('player-mode').selectOption('hls')
  await playing(page)

  // 低延迟模式（HTTP-FLV）
  await page.getByTestId('player-mode').selectOption('flv')
  await playing(page)

  // 超低延迟（WebRTC）：Chrome 与 Firefox
  if (browserName !== 'webkit') {
    await page.getByTestId('player-mode').selectOption('webrtc')
    await playing(page)
  }
})
