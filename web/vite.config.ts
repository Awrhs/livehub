import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// 开发时把 API / 播放 / WebSocket 代理到本地 LiveHub（默认 :8000）
const target = process.env.LIVEHUB_DEV_TARGET || 'http://127.0.0.1:8000'

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': { target, ws: true, changeOrigin: false },
      '/live': { target },
      '/thumbs': { target },
      '/vods': { target },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: false,
    chunkSizeWarningLimit: 1200,
  },
})
