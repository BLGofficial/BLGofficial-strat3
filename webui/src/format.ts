// Number and label formatting shared by the panels. Plain "-" and no thousands separators,
// to read the same as the reason text the plugin writes.

/** Removes the sign of a rounded zero: "-0.0" reads like a value and is not one. */
const unsigned = (text: string): string => (Number(text) === 0 ? text.replace('-', '') : text)

/** A depth of 0..1 as a whole percentage, e.g. "60 %". */
export const percent = (depth: number): string => `${Math.round(depth * 100)} %`

/** A parameter value without float noise: at most 3 decimals, no trailing zeros. */
export const plain = (value: number): string => unsigned(String(Number(value.toFixed(3))))

/** One decimal, e.g. "-17.2". */
export const fixed1 = (value: number): string => unsigned(value.toFixed(1))

/**
 * Whole seconds of a capture done, e.g. "12 / 30 s".
 * The plugin's progress is a float32, so 42 s of 60 arrives as 0.69999999. The 0.01 s
 * nudge is under a third of one analysis frame (1/30 s) and so never shows a second early.
 */
export const elapsed = (progress: number, seconds: number): string => `${Math.floor(progress * seconds + 0.01)} / ${seconds} s`

/** A layer id as its row label in the native stack, e.g. "eq" as "EQ". */
export const layerName = (layer: string): string => layer.toUpperCase()

/** An axis frequency, e.g. 100 as "100" and 2500 as "2.5k". */
export const axisHz = (hz: number): string => (hz >= 1000 ? `${Number((hz / 1000).toFixed(1))}k` : String(hz))
