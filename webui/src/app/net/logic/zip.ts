// A minimal zip writer (stored entries: WAV barely compresses) and the CSV
// that goes with the audio download.

let table: Uint32Array | null = null;

export function crc32(u8: Uint8Array): number {
  if (!table) {
    table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
  }
  let x = 0xffffffff;
  for (let i = 0; i < u8.length; i++) x = table[(x ^ u8[i]) & 255] ^ (x >>> 8);
  return (x ^ 0xffffffff) >>> 0;
}

export interface ZipEntry { name: string; data: Uint8Array; }

/** The zip as bytes (UTF-8 names, local time stamps). */
export function zipBytes(files: ZipEntry[], when = new Date()): Uint8Array {
  const enc = new TextEncoder();
  const dtime = (when.getHours() << 11) | (when.getMinutes() << 5) | (when.getSeconds() >> 1);
  const ddate = ((when.getFullYear() - 1980) << 9) | ((when.getMonth() + 1) << 5) | when.getDate();
  const parts: Uint8Array[] = [], cdir: Uint8Array[] = [];
  let off = 0, cdLen = 0;
  for (const f of files) {
    const nm = enc.encode(f.name), crc = crc32(f.data), n = f.data.length;
    const lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true);
    lh.setUint16(10, dtime, true); lh.setUint16(12, ddate, true); lh.setUint32(14, crc, true);
    lh.setUint32(18, n, true); lh.setUint32(22, n, true); lh.setUint16(26, nm.length, true);
    parts.push(new Uint8Array(lh.buffer), nm, f.data);
    const ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true);
    ch.setUint16(12, dtime, true); ch.setUint16(14, ddate, true); ch.setUint32(16, crc, true);
    ch.setUint32(20, n, true); ch.setUint32(24, n, true); ch.setUint16(28, nm.length, true); ch.setUint32(42, off, true);
    cdir.push(new Uint8Array(ch.buffer), nm);
    off += 30 + nm.length + n;
    cdLen += 46 + nm.length;
  }
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, files.length, true); end.setUint16(10, files.length, true);
  end.setUint32(12, cdLen, true); end.setUint32(16, off, true);
  const all = [...parts, ...cdir, new Uint8Array(end.buffer)];
  const out = new Uint8Array(all.reduce((s, p) => s + p.length, 0));
  let at = 0;
  for (const p of all) { out.set(p, at); at += p.length; }
  return out;
}

/** One CSV cell: quoted when needed; a leading = + - @ is defused (spreadsheet formulas). */
export function csvCell(v: unknown): string {
  let s = v == null ? '' : String(v);
  if (/^[=+\-@]/.test(s)) s = "'" + s;
  return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

/** CSV text (with a BOM, so spreadsheets read UTF-8) from rows of cells. */
export function csv(rows: unknown[][]): string {
  return '﻿' + rows.map((r) => r.map(csvCell).join(',')).join('\r\n') + '\r\n';
}
