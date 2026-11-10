// Number and label formatting for the panels. Contracts:
//   - percent: a depth of 0..1 as a whole percentage
//   - plain: a parameter value without float noise, at most 3 decimals, never "-0"
//   - fixed1: one decimal, never "-0.0"
//   - elapsed: whole seconds of a capture, robust to the float32 progress the plugin sends
//     (0.7f * 60 is 41.9999993, which is 42 s of audio)
//   - layerName / axisHz: short upper-case labels
import { describe, expect, it } from 'vitest'

import { axisHz, elapsed, fixed1, hz, layerName, percent, plain } from './format'

describe('format', () => {
  it.each([
    [0, '0 %'],
    [0.6000000238418579, '60 %'],
    [0.346, '35 %'],
    [1, '100 %'],
  ])('percent(%s) is %s', (depth, text) => {
    expect(percent(depth)).toBe(text)
  })

  it.each([
    [-3.0399999618530273, '-3.04'],
    [4000.000001, '4000'],
    [0.7070000171661377, '0.707'],
    [250, '250'],
    [-0, '0'],
    [-0.0001, '0'],
    [1.7135, '1.714'],
  ])('plain(%s) is %s', (value, text) => {
    expect(plain(value)).toBe(text)
  })

  it.each([
    [4.34, '4.3'],
    [-17.2, '-17.2'],
    [-0.04, '0.0'],
    [0, '0.0'],
    [-10, '-10.0'],
  ])('fixed1(%s) is %s', (value, text) => {
    expect(fixed1(value)).toBe(text)
  })

  it.each([
    [0, 30, '0 / 30 s'],
    [0.41333332657814, 30, '12 / 30 s'],
    [0.699999988079071, 60, '42 / 60 s'],
    [899 / 900, 30, '29 / 30 s'], // one analysis frame short of the end
    [1, 30, '30 / 30 s'],
  ])('elapsed(%s, %s) is %s', (progress, seconds, text) => {
    expect(elapsed(progress, seconds)).toBe(text)
  })

  it('names layers in capitals', () => {
    expect(layerName('eq')).toBe('EQ')
    expect(layerName('limit')).toBe('LIMIT')
  })

  it.each([
    [20, '20'],
    [100, '100'],
    [1000, '1k'],
    [10000, '10k'],
    [2500, '2.5k'],
  ])('axisHz(%s) is %s', (hz, text) => {
    expect(axisHz(hz)).toBe(text)
  })
})

describe('hz (module 21d)', () => {
  it('is whole Hz under 1 kHz and kHz with two decimals from there', () => {
    expect(hz(120)).toBe('120 Hz')
    expect(hz(299.99997)).toBe('300 Hz')
    expect(hz(999.4)).toBe('999 Hz')
    expect(hz(999.6)).toBe('1.00 kHz')
    expect(hz(1000)).toBe('1.00 kHz')
    expect(hz(4999.9995)).toBe('5.00 kHz')
    expect(hz(16000)).toBe('16.00 kHz')
  })
})
