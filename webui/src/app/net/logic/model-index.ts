import { NetCall, NetFamily, NetNetwork, NetRadio, NetTalkgroup } from '../../core/models';

/** One protocol family's data, indexed for the views. */
export interface NetIndex {
  family: NetFamily;
  /** Networks, oldest first (the order colours are handed out in). */
  nets: NetNetwork[];
  netByKey: Record<string, NetNetwork>;
  tgs: NetTalkgroup[];
  tgById: Record<string, NetTalkgroup>;
  radios: NetRadio[];
  rById: Record<string, NetRadio>;
  calls: NetCall[];
  /** Talkgroups / radios per network. */
  netTg: Record<string, number>;
  netRad: Record<string, number>;
  /** Networks ranked by calls (0 = busiest): a node's "primary" network is its busiest. */
  netRank: Record<string, number>;
}

export function buildIndex(F: NetFamily): NetIndex {
  const X: NetIndex = {
    family: F, nets: F.networks.slice().sort((a, b) => a.first - b.first), netByKey: {},
    tgs: F.talkgroups, tgById: {}, radios: F.radios, rById: {}, calls: F.calls, netTg: {}, netRad: {}, netRank: {},
  };
  for (const n of X.nets) X.netByKey[n.key] = n;
  for (const t of X.tgs) {
    X.tgById[t.id] = t;
    for (const k of t.networks) X.netTg[k] = (X.netTg[k] || 0) + 1;
  }
  for (const r of X.radios) {
    X.rById[r.id] = r;
    for (const k of r.networks) X.netRad[k] = (X.netRad[k] || 0) + 1;
  }
  X.nets.slice().sort((a, b) => b.calls - a.calls).forEach((n, i) => (X.netRank[n.key] = i));
  return X;
}

/** The busiest of `list`'s networks. */
export function primaryNet(ix: NetIndex, list: string[] | undefined): string | null {
  let best: string | null = null;
  for (const k of list || [])
    if (best == null || (ix.netRank[k] || 0) < (ix.netRank[best] || 0)) best = k;
  return best;
}

/** The most recent alias of a radio. */
export function aliasOf(ix: NetIndex, id: string): string {
  const r = ix.rById[id];
  return r && r.aliases.length ? r.aliases[r.aliases.length - 1] : '';
}

export const PALETTE = ['#5bc0de', '#62c462', '#f89406', '#ee5f5b', '#b38bff', '#e6c229', '#3fc1a5', '#ff7eb6',
  '#8fa8ff', '#c3e88d', '#ffab70', '#4dd0e1', '#d4a5ff', '#a3d977'];
export const UNIDENTIFIED_COLOR = '#7a8288';

/**
 * Stable network colours: each network of a protocol gets the next palette
 * colour the first time it is seen, and keeps it while the page is open.
 */
export class NetColors {
  private readonly assigned = new Map<string, string>();
  private readonly next = new Map<string, number>();

  colorFor(fam: string, net: NetNetwork | undefined): string {
    if (!net || net.confidence === 'none') return UNIDENTIFIED_COLOR;
    const k = fam + '|' + net.key;
    let c = this.assigned.get(k);
    if (!c) {
      const i = this.next.get(fam) || 0;
      c = PALETTE[i % PALETTE.length];
      this.assigned.set(k, c);
      this.next.set(fam, i + 1);
    }
    return c;
  }
  /** Hands out colours in network order (oldest first), so they don't depend on what renders first. */
  assign(fam: string, ix: NetIndex): void {
    for (const n of ix.nets) this.colorFor(fam, n);
  }
}
