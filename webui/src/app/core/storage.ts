// localStorage that never throws (private windows, blocked site data); the
// explorer's keys keep the built-in page's "netx." prefix so settings carry over.
export const store = {
  get(key: string): string | null {
    try { return localStorage.getItem('netx.' + key); } catch { return null; }
  },
  set(key: string, value: string): void {
    try { localStorage.setItem('netx.' + key, value); } catch { /* ignore */ }
  },
};
