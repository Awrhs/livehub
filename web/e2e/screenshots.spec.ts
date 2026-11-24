import { test, expect, type APIRequestContext, type Browser, type Page } from '@playwright/test'
import { spawn, type ChildProcess } from 'node:child_process'
import { mkdirSync } from 'node:fs'

// 生成 README 截图：准备演示数据（多个直播中的频道、聊天、回放、待审批申请）后截图。
// 仅在 SCREENSHOTS=1 时运行（由 .github/workflows/screenshots.yml 触发），常规 E2E 跳过。
test.skip(!process.env.SCREENSHOTS, '仅在生成截图时运行')

const OUT = process.env.SCREENSHOT_DIR || 'screenshots'
const RTMP = 'rtmp://127.0.0.1:1935/live'
const H = { 'X-Requested-With': 'livehub' }
const pushes: ChildProcess[] = []

function push(key: string, source: string, seconds: number) {
  const p = spawn('ffmpeg', [
    '-hide_banner', '-loglevel', 'error', '-re',
    '-f', 'lavfi', '-i', source,
    '-f', 'lavfi', '-i', 'sine=frequency=330:sample_rate=44100',
    '-t', String(seconds),
    '-c:v', 'libx264', '-preset', 'veryfast', '-g', '50', '-pix_fmt', 'yuv420p', '-b:v', '1500k',
    '-c:a', 'aac', '-b:a', '64k', '-f', 'flv', `${RTMP}/${key}`,
  ], { stdio: 'inherit' })
  pushes.push(p)
  return p
}

async function user(browser: Browser, username: string, display: string): Promise<{ page: Page; api: APIRequestContext; me: any }> {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, colorScheme: 'dark', locale: 'zh-CN' })
  const page = await ctx.newPage()
  const r = await page.request.post('/api/v1/auth/register', {
    headers: H, data: { username, password: 'password123', display_name: display },
  })
  expect(r.ok(), `注册 ${username}: ${r.status()} ${await r.text()}`).toBeTruthy()
  return { page, api: page.request, me: await r.json() }
}

async function waitFor(fn: () => Promise<boolean>, ms = 60_000) {
  const end = Date.now() + ms
  while (Date.now() < end) {
    if (await fn()) return
    await new Promise((r) => setTimeout(r, 1000))
  }
  throw new Error('等待超时')
}

test.afterAll(() => {
  for (const p of pushes) p.kill('SIGINT')
})

test('README 截图', async ({ browser }) => {
  test.setTimeout(600_000)
  mkdirSync(OUT, { recursive: true })

  // ---- 演示数据 ----
  const admin = await user(browser, 'zhanzhang', '站长')
  await admin.api.put('/api/v1/admin/settings', {
    headers: H,
    data: { site_name: 'LiveHub 社区', announcement: '🎉 欢迎来到 LiveHub！本周六晚 8 点举办社区直播活动，欢迎各位主播报名。' },
  })

  const streamers = [
    { u: 'gopher', n: '地鼠程序员', title: '周末编程马拉松：用 Go 写一个直播平台', cat: '编程', src: 'testsrc2=size=1280x720:rate=25' },
    { u: 'pixelart', n: '像素小画家', title: '像素画速绘 · 今天画一座赛博城市', cat: '绘画', src: 'mandelbrot=size=1280x720:rate=25' },
    { u: 'indiegame', n: '独立游戏研究所', title: '独立游戏试玩：生命游戏模拟器', cat: '游戏', src: 'life=s=1280x720:mold=10:r=25:ratio=0.3:death_color=#1e1b4b:life_color=#a78bfa' },
    { u: 'citynight', n: '城市夜行', title: '24 小时城市慢直播', cat: '生活', src: 'smptehdbars=size=1280x720:rate=25' },
  ]
  const accounts: Record<string, Awaited<ReturnType<typeof user>>> = {}
  for (const s of streamers) {
    const a = await user(browser, s.u, s.n)
    accounts[s.u] = a
    await a.api.post('/api/v1/me/stream-request', { headers: H, data: { note: '想和大家分享' + s.cat } })
    await admin.api.patch(`/api/v1/admin/users/${a.me.id}`, { headers: H, data: { stream_status: 'approved' } })
    await a.api.patch('/api/v1/me/channel', {
      headers: H, data: { title: s.title, category: s.cat, description: `这里是${s.n}的直播间，欢迎关注！` },
    })
  }
  // 一个待审批的申请（管理后台截图用）
  const newbie = await user(browser, 'newbie', '新人主播')
  await newbie.api.post('/api/v1/me/stream-request', { headers: H, data: { note: '我是大学摄影社的，想直播校园活动。' } })

  // 先推一段短流生成回放（录像需 ≥3 秒）
  const vodKey = (await (await accounts.gopher.api.post('/api/v1/me/channel/key', { headers: H })).json()).new_key.publish_key
  const vodPush = push(vodKey, 'testsrc2=size=1280x720:rate=25', 15)
  await new Promise((r) => vodPush.on('exit', r))
  await waitFor(async () => (await (await admin.api.get('/api/v1/channels/gopher/vods')).json()).vods.length > 0)
  const vod = (await (await admin.api.get('/api/v1/channels/gopher/vods')).json()).vods[0]
  await accounts.gopher.api.patch(`/api/v1/vods/${vod.id}`, { headers: H, data: { title: '上周回放：从零搭建推流鉴权' } })

  // 四个频道同时开播
  for (const s of streamers) {
    const me = await (await accounts[s.u].api.post('/api/v1/me/channel/key', { headers: H })).json()
    push(me.new_key.publish_key, s.src, 300)
  }
  for (const s of streamers) {
    await waitFor(async () => (await (await admin.api.get('/api/v1/channels/' + s.u)).json()).channel.thumbnail)
  }

  // 观众关注与聊天
  const viewers = [await user(browser, 'xiaoming', '小明'), await user(browser, 'alice', 'Alice')]
  for (const v of viewers) await v.api.post('/api/v1/channels/gopher/follow', { headers: H })

  // ---- 截图 1：首页 ----
  await admin.page.goto('/')
  await expect(admin.page.getByTestId('live-grid')).toBeVisible()
  await admin.page.waitForTimeout(2000)
  await admin.page.screenshot({ path: `${OUT}/home.png` })

  // ---- 截图 2：直播间（播放 + 聊天）----
  const watcher = viewers[0].page
  await watcher.goto('/gopher')
  const video = watcher.getByTestId('video')
  await expect.poll(() => video.evaluate((v: HTMLVideoElement) => v.currentTime), { timeout: 60_000 }).toBeGreaterThan(3)
  const chatLines: [Page, string][] = [
    [accounts.gopher.page, '大家好，今天继续写推流鉴权的部分 👋'],
    [viewers[0].page, '来了来了！'],
    [viewers[1].page, 'Hello from the other side 🙌'],
    [admin.page, '欢迎新朋友，记得点关注～'],
    [viewers[0].page, 'webhook 失败时为什么要拒绝推流？'],
    [accounts.gopher.page, '好问题：平台挂了的时候如果放行，任何人都能推流到你的频道'],
    [viewers[1].page, '学到了 👍'],
  ]
  for (const [p, text] of chatLines) {
    if (p.url().indexOf('/gopher') < 0) await p.goto('/gopher')
    await p.getByTestId('chat-input').fill(text)
    await p.getByTestId('chat-input').press('Enter')
    await watcher.waitForTimeout(400)
  }
  await expect(watcher.getByTestId('chat-msg')).toHaveCount(chatLines.length)
  await watcher.mouse.move(10, 10) // 移开鼠标，隐藏播放器悬浮控件
  await watcher.waitForTimeout(1500)
  await watcher.screenshot({ path: `${OUT}/channel.png` })

  // ---- 截图 3：主播后台 ----
  const gp = accounts.gopher.page
  await gp.goto('/dashboard')
  await expect(gp.getByTestId('publish-key')).toBeVisible()
  await gp.waitForTimeout(1000)
  await gp.screenshot({ path: `${OUT}/dashboard.png` })

  // ---- 截图 4：管理后台（开播审批）----
  await admin.page.goto('/admin')
  await admin.page.getByTestId('tab-approvals').click()
  await expect(admin.page.getByTestId('user-row-newbie')).toBeVisible()
  await admin.page.screenshot({ path: `${OUT}/admin.png` })

  // ---- 截图 5：频道回放列表 ----
  await viewers[1].page.goto('/gopher')
  await viewers[1].page.getByTestId('tab-vods').click()
  await expect(viewers[1].page.getByTestId('vod-card-' + vod.id)).toBeVisible()
  await viewers[1].page.waitForTimeout(1000)
  await viewers[1].page.screenshot({ path: `${OUT}/vods.png` })

  // ---- 截图 6：手机端直播间 ----
  const mctx = await browser.newContext({
    viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true,
    colorScheme: 'dark', locale: 'zh-CN',
  })
  const m = await mctx.newPage()
  await m.goto('/pixelart')
  await expect.poll(() => m.getByTestId('video').evaluate((v: HTMLVideoElement) => v.currentTime), { timeout: 60_000 }).toBeGreaterThan(2)
  await m.waitForTimeout(1500)
  await m.screenshot({ path: `${OUT}/mobile.png` })
})
