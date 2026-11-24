import { defineConfig } from '@playwright/test'

// 浏览器兼容性：Chrome / Firefox / WebKit（Safari 内核）分别验证各播放模式。
// 推流使用 OBS 默认风格的 x264 参数（High profile、含 B 帧）。
export default defineConfig({
  testDir: './e2e',
  testMatch: 'compat.spec.ts',
  timeout: 180_000,
  expect: { timeout: 30_000 },
  workers: 1,
  reporter: [['list']],
  use: {
    baseURL: process.env.LIVEHUB_URL || 'http://127.0.0.1:8000',
    headless: true,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    { name: 'chrome', use: { channel: 'chrome' } },
    { name: 'firefox', use: { browserName: 'firefox' } },
    { name: 'webkit', use: { browserName: 'webkit' } },
  ],
})
