// Pure formatting helpers shared by the status page and the explorer (ports
// of the functions in the server's built-in pages). Times are UTC.

export function p2(n: number): string {
  return (n < 10 ? '0' : '') + n;
}

/** 12:34:56 (UTC). */
export function hms(ms: number): string {
  const d = new Date(ms);
  return p2(d.getUTCHours()) + ':' + p2(d.getUTCMinutes()) + ':' + p2(d.getUTCSeconds());
}

/** 2026-10-02 12:34:56Z */
export function dt(ms: number): string {
  const d = new Date(ms);
  return d.getUTCFullYear() + '-' + p2(d.getUTCMonth() + 1) + '-' + p2(d.getUTCDate()) + ' ' + hms(ms) + 'Z';
}

/** 20261002T123456Z -- for file names. */
export function stamp(ms: number): string {
  return dt(ms).replace(/[-:]/g, '').replace(' ', 'T');
}

/** "12s ago", "3m ago", "2h ago", "4d ago"; "-" for no time. */
export function ago(ms: number, now: number): string {
  if (!ms) return '-';
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.floor(s / 60) + 'm ago';
  if (s < 86400) return Math.floor(s / 3600) + 'h ago';
  return Math.floor(s / 86400) + 'd ago';
}

/** A call's length: "<1s", "2.4s", "37s", "3m 05s". */
export function dur(ms: number): string {
  const s = Math.max(0, ms) / 1000;
  if (s < 1) return '<1s';
  if (s < 10) return s.toFixed(1) + 's';
  if (s < 60) return Math.round(s) + 's';
  const m = Math.floor(s / 60);
  return m + 'm ' + p2(Math.round(s - m * 60)) + 's';
}

/** The status page's "1h 23m 45s" / "23m 45s" / "45s". */
export function humanDuration(seconds: number): string {
  let s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  s -= h * 3600;
  const m = Math.floor(s / 60);
  s -= m * 60;
  let o = '';
  if (h) o += h + 'h ';
  if (h || m) o += m + 'm ';
  return o + s + 's';
}

/** 0:07 -- audio lengths. */
export function mmss(ms: number): string {
  const s = Math.round(ms / 1000);
  return Math.floor(s / 60) + ':' + p2(s % 60);
}

/** A frequency in MHz with at least 4 decimals: 460025000 -> "460.0250", 851012500 -> "851.0125". */
export function mhz(hz: number | null | undefined): string {
  if (!hz) return '';
  let t = (hz / 1e6).toFixed(6);
  while (t.length > t.indexOf('.') + 5 && t.charAt(t.length - 1) === '0') t = t.slice(0, -1);
  return t;
}

/** 1.4 MB / 312 KB */
export function mb(n: number): string {
  return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
}

/**
 * "dmr → nxdn48 ✗ → pager-auto": every protocol a session asked for, in order,
 * ✗ on requests whose pipeline never started; the current protocol when none
 * was recorded.
 */
export function protocolTrail(requested: string[] | undefined, used: string[] | undefined, current: string): string {
  const u = used || [];
  const list = requested && requested.length ? requested : u;
  if (!list.length) return current || '-';
  return list.map((p) => (u.indexOf(p) < 0 ? p + ' ✗' : p)).join(' → ');
}

/** Object keys, tolerating null / undefined. */
export function keys(o: Record<string, unknown> | null | undefined): string[] {
  return o ? Object.keys(o) : [];
}
