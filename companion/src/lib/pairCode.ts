/** Pocket pairing codes: 8 characters from the device's alphabet (no 0/1/I/O). */
export const PAIR_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'

export function normalizePairCode(raw: string): string {
  return raw.toUpperCase().replace(/[^A-Z0-9]/g, '')
}

export function isPairCode(code: string): boolean {
  return code.length === 8 && [...code].every((ch) => PAIR_ALPHABET.includes(ch))
}

/**
 * Read a pairing code from what Pocket's QR encodes — a Companion link
 * (`…/link?code=ABCD2345`) — or from a bare code. Returns null for anything else.
 */
export function pairCodeFromScan(text: string): string | null {
  const t = text.trim()
  try {
    const u = new URL(t)
    const c = normalizePairCode(u.searchParams.get('code') || '')
    return isPairCode(c) ? c : null
  } catch {
    const c = normalizePairCode(t)
    return isPairCode(c) ? c : null
  }
}
