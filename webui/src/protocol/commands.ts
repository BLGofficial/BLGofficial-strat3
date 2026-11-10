// Commands the page sends to the plugin: one object per user action, parsed strictly on
// the native side (source/assistant/Protocol.h, parseCommand). contract/commands.json pins
// these builders and that parser to the same objects.

/** The two capture lengths the plugin accepts. */
export type CaptureSeconds = 30 | 60

export type Command =
  | { readonly type: 'analyse'; readonly seconds: CaptureSeconds }
  | { readonly type: 'captureReference'; readonly seconds: CaptureSeconds }
  | { readonly type: 'cancel' }
  | { readonly type: 'accept'; readonly id: number }
  | { readonly type: 'reject'; readonly id: number }
  | { readonly type: 'depth'; readonly id: number; readonly depth: number }
  | { readonly type: 'applyAll' }
  | { readonly type: 'clearTarget' }

/** Capture the plugin's output and propose moves. */
export const analyse = (seconds: CaptureSeconds): Command => ({ type: 'analyse', seconds })

/** Capture what is playing through the plugin as one more reference for the target. */
export const captureReference = (seconds: CaptureSeconds): Command => ({ type: 'captureReference', seconds })

/** Stop the running capture; nothing is learned from it. */
export const cancel = (): Command => ({ type: 'cancel' })

/** Accept a pending or rejected move at the depth the plugin holds for it. */
export const accept = (id: number): Command => ({ type: 'accept', id })

/** Reject a move. An accepted one is undone: its parameters go back to their values at analysis time. */
export const reject = (id: number): Command => ({ type: 'reject', id })

/**
 * Set a move's depth, clamped to 0..1. A value that is not a number is left as it is: JSON
 * turns it into null and the plugin refuses the command, so bad input changes nothing.
 */
export const depth = (id: number, value: number): Command => ({ type: 'depth', id, depth: Math.min(1, Math.max(0, value)) })

/** Accept every pending move at its own depth. */
export const applyAll = (): Command => ({ type: 'applyAll' })

/** Forget every reference and the target built from them. */
export const clearTarget = (): Command => ({ type: 'clearTarget' })
