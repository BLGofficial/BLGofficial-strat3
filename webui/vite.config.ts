import react from '@vitejs/plugin-react'
import { defineConfig } from 'vitest/config'

// The bundle is embedded in the plugin (juce_add_binary_data) and served by JUCE's
// ResourceProvider, never from a web server or the network:
//   base './'       the page is addressed relative to the provider's root
//   fixed names     the native side maps each path to one embedded resource, so a content
//                   hash in the name (cache busting) would only make that mapping fragile
//   one chunk       nothing to load lazily from disk
// Target: the WebView is the system's (WKWebView, WebView2, WebKitGTK), so it can be years
// older than a current browser. safari14 is own choice [INFERRED]; the minimum macOS is
// not decided yet.
export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    target: ['es2020', 'chrome87', 'safari14'],
    cssCodeSplit: false,
    sourcemap: false,
    assetsInlineLimit: 0,
    modulePreload: false,
    rolldownOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.{ts,tsx}'],
    setupFiles: ['src/test/setup.ts'],
    restoreMocks: true,
  },
})
