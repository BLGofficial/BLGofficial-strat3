// Structural sharing for the state. The plugin sends the whole session on every change,
// up to 30 times a second while a capture runs, and each arrives as brand new objects.
// Without this, React would see 64 changed move rows per frame when only the progress moved.

const isPlainObject = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v)

/**
 * Merges a new JSON value over the previous one, keeping the previous object wherever the
 * content is the same.
 * @returns a value equal to `next`; `previous` itself if nothing differs
 */
export function reuse<T>(previous: T, next: T): T {
  if (Object.is(previous, next)) return previous

  if (Array.isArray(previous) && Array.isArray(next)) {
    const merged = next.map((item, i) => (i < previous.length ? reuse<unknown>(previous[i], item) : item))
    const same = previous.length === next.length && merged.every((item, i) => item === previous[i])
    return (same ? previous : merged) as T
  }

  if (isPlainObject(previous) && isPlainObject(next)) {
    const keys = Object.keys(next)
    const merged: Record<string, unknown> = {}
    let same = keys.length === Object.keys(previous).length
    for (const key of keys) {
      merged[key] = key in previous ? reuse(previous[key], next[key]) : next[key]
      same = same && key in previous && merged[key] === previous[key]
    }
    return (same ? previous : merged) as T
  }

  return next
}
