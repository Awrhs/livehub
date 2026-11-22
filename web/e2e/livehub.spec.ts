import { test, expect, type Browser, type Page } from '@playwright/test'
import { spawn, type ChildProcess } from 'node:child_process'

// 完整用户旅程：管理员初始化站点 → 主播注册并申请开播 → 管理员审批 →
// 主播用推流码推流（ffmpeg）→ 观众在浏览器中观看并聊天 → 关注 → 下播。

const RTMP = process.env.LIVEHUB_RTMP || 'rtmp://127.0.0.1:1935/live'
const PASSWORD = 'password123'

async function newPage(browser: Browser): Promise<Page> {
  const ctx = await browser.newContext()
  return ctx.newPage()
}

async function register(page: Page, username: string, displayName = '') {
  await page.goto('/register')
  await page.locator('input[name=username]').fill(username)
  if (displayName) await page.locator('input[name=display_name]').fill(displayName)
  await page.locator('input[name=password]').fill(PASSWORD)
  await page.locator('form button[type=submit]').click()
  await expect(page.getByTestId('user-menu')).toBeVisible()
}

function startPush(publishKey: string, seconds: number): ChildProcess {
  return spawn(
    'ffmpeg',
    [
      '-hide_banner', '-loglevel', 'error', '-re',
      '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=25',
      '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100',
      '-t', String(seconds),
      '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency', '-g', '50', '-pix_fmt', 'yuv420p',
      '-c:a', 'aac', '-b:a', '64k',
      '-f', 'flv', `${RTMP}/${publishKey}`,
    ],
    { stdio: 'inherit' },
  )
}

// 视频确实在播放：currentTime 持续前进
async function expectPlaying(page: Page) {
  const video = page.getByTestId('video')
  await expect(video).toBeVisible()
  await expect
    .poll(async () => video.evaluate((v: HTMLVideoElement) => v.currentTime), { timeout: 45_000, intervals: [1000] })
    .toBeGreaterThan(2)
  const t1 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  await page.waitForTimeout(2000)
  const t2 = await video.evaluate((v: HTMLVideoElement) => v.currentTime)
  expect(t2).toBeGreaterThan(t1)
  const size = await video.evaluate((v: HTMLVideoElement) => [v.videoWidth, v.videoHeight])
  expect(size).toEqual([640, 360])
}

test('站点完整用户旅程', async ({ browser }) => {
  // 1. 首页提示初始化；首个注册用户成为管理员并进入管理后台
  const admin = await newPage(browser)
  await admin.goto('/')
  await expect(admin.getByText('创建管理员账号')).toBeVisible()
  await register(admin, 'admin1', '站长')
  await expect(admin).toHaveURL(/\/admin$/)
  await expect(admin.getByTestId('tab-approvals')).toBeVisible()

  // 2. 主播注册后在主播后台申请开播
  const streamer = await newPage(browser)
  await register(streamer, 'streamer1', '主播小明')
  await expect(streamer).toHaveURL(/\/dashboard$/)
  const request = streamer.getByTestId('stream-request')
  await request.locator('textarea').fill('我想直播编程')
  await request.locator('button[type=submit]').click()
  await expect(streamer.getByText('等待管理员审批')).toBeVisible()

  // 3. 管理员在「开播审批」中批准
  await admin.getByTestId('tab-approvals').click()
  const row = admin.getByTestId('user-row-streamer1')
  await expect(row).toBeVisible()
  await row.getByTestId('approve').click()
  await expect(row).toHaveCount(0)

  // 4. 主播刷新后看到推流设置，修改直播标题
  await streamer.reload()
  await expect(streamer.getByTestId('ingest-url')).toHaveValue(/^rtmp:\/\//)
  const publishKey = await streamer.getByTestId('publish-key').inputValue()
  expect(publishKey).toMatch(/^streamer1\?key=lh_/)
  await streamer.getByTestId('channel-title-input').fill('E2E 测试直播')
  await streamer.getByTestId('channel-title-input').press('Enter')
  await expect(streamer.getByText('已保存')).toBeVisible()

  // 5. 观众（游客）打开频道页：未开播
  const viewer = await newPage(browser)
  await viewer.goto('/streamer1')
  await expect(viewer.getByTestId('offline')).toBeVisible()
  await expect(viewer.getByTestId('channel-title')).toHaveText('E2E 测试直播')

  // 6. 开始推流：频道页自动切换为直播并开始播放（无需刷新）
  const push = startPush(publishKey, 90)
  try {
    await expect(viewer.getByTestId('live-badge')).toBeVisible({ timeout: 30_000 })
    await expectPlaying(viewer)

    // 首页「正在直播」出现该频道
    const home = await newPage(browser)
    await home.goto('/')
    await expect(home.getByTestId('channel-card-streamer1')).toBeVisible({ timeout: 25_000 })

    // 7. 低延迟模式（HTTP-FLV）同样可播放
    await viewer.getByTestId('player-mode').selectOption('flv')
    await expectPlaying(viewer)

    // 7b. 超低延迟模式（WebRTC/WHEP）：视频播放且带音频轨（AAC→Opus 转码）
    await viewer.getByTestId('player-mode').selectOption('webrtc')
    await expectPlaying(viewer)
    await expect
      .poll(async () =>
        viewer.getByTestId('video').evaluate((v: HTMLVideoElement) => {
          const s = v.srcObject as MediaStream | null
          return s ? s.getAudioTracks().length : 0
        }),
      )
      .toBe(1)

    // 8. 管理员进入频道发言，游客实时看到；游客看到登录提示
    await admin.goto('/streamer1')
    await expect(admin.getByTestId('chat-input')).toBeVisible()
    await admin.getByTestId('chat-input').fill('欢迎来到直播间！')
    await admin.getByTestId('chat-input').press('Enter')
    await expect(viewer.getByTestId('chat-msg').filter({ hasText: '欢迎来到直播间！' })).toBeVisible()
    await expect(viewer.getByText('登录后参与聊天')).toBeVisible()

    // 9. 管理员关注主播
    await admin.getByTestId('follow-btn').click()
    await expect(admin.getByTestId('follow-btn')).toContainText('已关注')

    // 10. 切换英文界面
    await viewer.getByRole('button', { name: 'EN', exact: true }).click()
    await expect(viewer.getByText('Log in to chat')).toBeVisible()
  } finally {
    push.kill('SIGINT')
  }

  // 11. 推流结束：频道页自动显示未开播
  await expect(viewer.getByTestId('offline')).toBeVisible({ timeout: 40_000 })
})
