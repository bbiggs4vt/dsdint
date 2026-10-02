import { NetExport } from '../../core/models';

/** A recording (raw decoder input) rather than an export. */
export function isRecording(text: string): boolean {
  return /^\s*\{"op":"header"/.test(text);
}

/** null when `d` is an explorer export the page can show, else why not. */
export function exportProblem(d: unknown, name: string): string | null {
  const x = d as NetExport | null;
  const ok = !!x && typeof x.families === 'object' && x.families !== null &&
    (x.format === 'dsd-net-export' || typeof x.now === 'number');
  return ok ? null : '"' + name + '" is not a dsd-server explorer export (expected format "dsd-net-export").';
}

/** "rx-north 10-02 07:00–07:30Z": an export source, briefly. */
export function sourceText(s: { name?: string; instance?: string; since?: number; through?: number }, dt: (ms: number) => string): string {
  const t = (ms?: number) => (ms ? dt(ms).slice(5, 16) : 'start');
  return (s.name || (s.instance || '').slice(0, 8) || '?') + ' ' + t(s.since) + '–' + t(s.through).slice(6) + 'Z';
}
