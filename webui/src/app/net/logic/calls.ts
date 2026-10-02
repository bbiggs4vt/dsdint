import { dt, mhz, stamp } from '../../core/format';
import { NetCall, Rate } from '../../core/models';

/** A call counts as live this long after its last event. */
export const LIVE_MS = 3000;

export function isLive(c: NetCall, now: number, fileView: boolean): boolean {
  return !fileView && c.open && now - c.last < LIVE_MS;
}

/** A call's identity across polls (its id can change as imports come and go). */
export function callKey(c: NetCall): string {
  return c.start + '|' + c.session + '|' + c.slot + '|' + c.src + '|' + c.tgt;
}

export function safeName(s: string): string {
  return String(s).replace(/[^A-Za-z0-9.-]+/g, '_');
}

/** A descriptive name for a call's audio file: when, where, to and from whom. */
export function callFile(c: NetCall): string {
  return 'call_' + stamp(c.start) + (c.freq ? '_' + mhz(c.freq) + 'MHz' : '') + (c.slot ? '_s' + c.slot : '') +
    '_' + (c.priv ? 'to_' : 'TG') + safeName(c.tgt || 'x') + '_from_' + safeName(c.src || 'x') + '.wav';
}

export interface RateView { text: string; tip: string; }

/**
 * Calls per second. Unfiltered live views use the server's count (every call,
 * however many are still listed); with a network / search filter or in a file
 * view it is worked out from the listed calls of the last minute.
 */
export function callRate(serverRate: Rate | undefined, filtered: boolean, calls: NetCall[], now: number,
                         onNetwork: boolean, searching: boolean): RateView {
  if (serverRate && !filtered)
    return {
      text: serverRate.per_s_1m.toFixed(1),
      tip: 'Calls per second over the last minute: ' + serverRate.per_s_1m.toFixed(2) + ' (last 10 minutes: ' +
        serverRate.per_s_10m.toFixed(2) + '). ' + serverRate.total + ' calls counted since the server started or was cleared.',
    };
  let n = 0, oldest = now;
  for (const c of calls) {
    if (c.start > now - 60000) ++n;
    if (c.start < oldest) oldest = c.start;
  }
  const span = Math.max(1, Math.min(60, (now - oldest) / 1000));
  return {
    text: (n / span).toFixed(1),
    tip: 'Calls per second over the last minute, from the calls listed' + (onNetwork ? ' on this network' : '') +
      (searching ? ' matching the search' : '') + ': ' + n + ' calls.',
  };
}

/**
 * Pause list: the calls held when it was pressed, refreshed from the current
 * data (durations, audio, transcripts move on) and the number of calls that
 * arrived since.
 */
export function heldCalls(held: NetCall[], heldKeys: ReadonlySet<string>, current: NetCall[],
                          freshFilter: (cs: NetCall[]) => NetCall[]): { list: NetCall[]; fresh: number } {
  const cur = new Map<string, NetCall>();
  for (const c of current) cur.set(callKey(c), c);
  return {
    list: held.map((c) => cur.get(callKey(c)) || c),
    fresh: freshFilter(current).filter((c) => !heldKeys.has(callKey(c))).length,
  };
}

/** UTC start, for the CSV. */
export const csvTime = dt;
