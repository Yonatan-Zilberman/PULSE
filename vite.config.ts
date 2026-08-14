/// <reference types="vitest/config" />
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: {
      ignored: ['**/src-tauri/**', '**/src-cpp/**'],
    },
  },
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: [],
  },
} as Parameters<typeof defineConfig>[0] & { test: Record<string, unknown> });
