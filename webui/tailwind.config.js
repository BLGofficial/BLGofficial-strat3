// Same tokens as the native editor (source/ui/native/Theme.h): monochrome base, one signal
// colour, square corners. Keep the two in step by hand; there are seven shared values.
/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    colors: {
      transparent: 'transparent',
      current: 'currentColor',
      base: '#0b0b0b',
      row: '#1a1a1a',
      'row-selected': '#242424',
      rule: '#2e2e2e',
      text: '#ededed',
      secondary: '#6b6b6b',
      // Web only. Small text in `secondary` is 3.7:1 on `base`; this grey is 7.2:1.
      muted: '#9b9b9b',
      signal: '#ff4a1c',
    },
    borderRadius: { none: '0', DEFAULT: '0' },
    fontFamily: {
      // System faces until the project embeds its own (Theme.h: not chosen yet).
      label: ['ui-sans-serif', 'system-ui', '"Helvetica Neue"', 'Arial', 'sans-serif'],
      mono: ['ui-monospace', '"SF Mono"', 'Menlo', 'Consolas', '"DejaVu Sans Mono"', 'monospace'],
    },
    extend: {
      fontSize: {
        label: ['11px', { lineHeight: '14px', letterSpacing: '0.08em' }],
        body: ['12px', { lineHeight: '16px' }],
      },
    },
  },
  plugins: [],
}
