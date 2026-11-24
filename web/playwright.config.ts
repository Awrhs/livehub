import { defineConfig } from '@playwright/test'

// 浏览器 E2E：由 platform/e2e/browser.sh 启动 LiveHub 后运行。
// 使用系统安装的 Google Chrome（channel: 'chrome'）：Playwright 自带的 Chromium 不含 H.264/AAC 解码器，无法播放直播。
export default defineConfig({
  testDir: './e2e',
  testMatch: ['livehub.spec.ts', 'screenshots.spec.ts'], // 截图测试仅在 SCREENSHOTS=1 时运行；兼容性测试使用独立配置
  timeout: 120_000,
  expect: { timeout: 20_000 },
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [['list'], ['html', { open: 'never', outputFolder: 'playwright-report' }]],
  use: {
    baseURL: process.env.LIVEHUB_URL || 'http://127.0.0.1:8000',
    channel: 'chrome',
    headless: true,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
    locale: 'zh-CN',
  },
})
