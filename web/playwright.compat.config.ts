import { defineConfig } from '@playwright/test'

// 浏览器兼容性：Chrome / Firefox / WebKit（Safari 内核）分别验证各播放模式。
// 推流使用 OBS 默认风格的 x264 参数（High profile、含 B 帧）。
//
// Firefox 的 WebRTC H.264 由 OpenH264 插件（GMP）提供：正式版 Firefox 首次启动后自动下载，
// Playwright 的 Firefox 构建不会下载。CI 中预先下载 Mozilla 官方分发的同一插件（OPENH264_GMP_DIR），
// 按 Linux 发行版的系统安装方式（MOZ_GMP_PATH + 偏好设置）提供给 Firefox，与真实用户环境一致。
// 另设一个不带插件的 Firefox 项目，验证此时播放器给出明确提示。
const gmpDir = process.env.OPENH264_GMP_DIR

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
    {
      name: 'firefox',
      use: {
        browserName: 'firefox',
        launchOptions: gmpDir
          ? {
              env: { ...process.env, MOZ_GMP_PATH: gmpDir, MOZ_DISABLE_GMP_SANDBOX: '1' } as Record<string, string>,
              firefoxUserPrefs: {
                'media.gmp-gmpopenh264.autoupdate': false,
                'media.gmp-gmpopenh264.version': 'system-installed',
                'media.gmp-gmpopenh264.enabled': true,
                'media.gmp-gmpopenh264.provider.enabled': true,
                'media.peerconnection.video.h264_enabled': true,
                'media.gmp-manager.updateEnabled': false,
              },
            }
          : undefined,
      },
    },
    // 不带 OpenH264 的 Firefox：WebRTC 无法协商 H.264，验证播放器的明确提示
    { name: 'firefox-noh264', use: { browserName: 'firefox' } },
    { name: 'webkit', use: { browserName: 'webkit' } },
  ],
})
