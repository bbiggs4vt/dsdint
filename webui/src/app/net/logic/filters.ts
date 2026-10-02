import { mhz } from '../../core/format';
import { NetCall, NetRadio, NetTalkgroup } from '../../core/models';
import { NetIndex } from './model-index';

export const ALL = '*';

/** What the search box, network chips and "With audio only" select. */
export interface ViewFilter {
  net: string;          // network key, or ALL
  q: string;            // search text ('' = none)
  audioOnly?: boolean;  // calls with audio only (live view)
}

/** Case-insensitive substring match against any of the values (arrays: any element). */
export function matchesQuery(q: string, ...vals: unknown[]): boolean {
  const needle = q.toLowerCase();
  for (const v of vals) {
    if (v == null) continue;
    if (Array.isArray(v)) {
      if (v.some((x) => String(x).toLowerCase().includes(needle))) return true;
    } else if (String(v).toLowerCase().includes(needle)) return true;
  }
  return false;
}

export function inNet(list: string[] | undefined, net: string): boolean {
  return net === ALL || (list || []).includes(net);
}

/** Calls matching the filter: network, search (ids, aliases, text, MHz) and audio. */
export function filterCalls(ix: NetIndex, f: ViewFilter, list: NetCall[] = ix.calls): NetCall[] {
  return list.filter((c) =>
    (f.net === ALL || c.net === f.net) && (!f.audioOnly || !!c.audio) &&
    (!f.q || matchesQuery(f.q, c.src, c.tgt, c.alias, c.text, mhz(c.freq), ix.rById[c.src]?.aliases)));
}

export function filterTalkgroups(ix: NetIndex, f: ViewFilter): NetTalkgroup[] {
  return ix.tgs.filter((t) => inNet(t.networks, f.net) && (!f.q || matchesQuery(f.q, t.id)));
}

export function filterRadios(ix: NetIndex, f: ViewFilter): NetRadio[] {
  return ix.radios.filter((r) => inNet(r.networks, f.net) && (!f.q || matchesQuery(f.q, r.id, r.aliases)));
}

export type SortKey<T> = (row: T) => string | number;

/** Sorts a copy; strings compare naturally ("TG 9" < "TG 10"), dir 1 ascending, -1 descending. Stable. */
export function sortRows<T>(rows: readonly T[], key: SortKey<T>, dir: number): T[] {
  return rows.slice().sort((a, b) => {
    const x = key(a), y = key(b);
    if (typeof x === 'string' && typeof y === 'string') return x.localeCompare(y, undefined, { numeric: true }) * dir;
    return (x < y ? -1 : x > y ? 1 : 0) * dir;
  });
}
