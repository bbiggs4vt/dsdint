import { keys } from '../../core/format';
import { NetCall, NetNetwork, NetRadio, NetTalkgroup } from '../../core/models';
import { NetIndex } from './model-index';

export interface Community { t: string[]; r: string[]; }

/**
 * Talk communities: connected components of the radio-talkgroup graph (with
 * private calls as radio-radio edges). Kept: two or more radios, or a radio
 * with a talkgroup; largest first.
 */
export function communities(tgs: NetTalkgroup[], radios: NetRadio[]): Community[] {
  const par: Record<string, string> = {};
  const find = (x: string): string => {
    while (par[x] !== x) { par[x] = par[par[x]]; x = par[x]; }
    return x;
  };
  const union = (a: string, b: string) => {
    a = find(a); b = find(b);
    if (a !== b) par[a] = b;
  };
  const tset = new Set(tgs.map((t) => t.id)), rset = new Set(radios.map((r) => r.id));
  for (const t of tgs) par['t:' + t.id] = 't:' + t.id;
  for (const r of radios) par['r:' + r.id] = 'r:' + r.id;
  for (const r of radios) {
    for (const t of keys(r.tgs)) if (tset.has(t)) union('r:' + r.id, 't:' + t);
    for (const p of keys(r.peers)) if (rset.has(p)) union('r:' + r.id, 'r:' + p);
  }
  const g: Record<string, Community> = {};
  for (const n of Object.keys(par)) {
    const c = (g[find(n)] ||= { t: [], r: [] });
    (n[0] === 't' ? c.t : c.r).push(n.slice(2));
  }
  return Object.values(g)
    .filter((c) => c.r.length >= 2 || (c.r.length >= 1 && c.t.length >= 1))
    .sort((a, b) => (b.r.length + b.t.length) - (a.r.length + a.t.length));
}

/** Networks a community touches. */
export function communityNetworks(c: Community, ix: NetIndex): string[] {
  const s = new Set<string>();
  for (const t of c.t) for (const k of ix.tgById[t]?.networks || []) s.add(k);
  for (const r of c.r) for (const k of ix.rById[r]?.networks || []) s.add(k);
  return [...s];
}

/** Hub radios: on three or more talkgroups, busiest first. */
export function hubRadios(radios: NetRadio[]): NetRadio[] {
  return radios.filter((r) => keys(r.tgs).length >= 3).sort((a, b) => keys(b.tgs).length - keys(a.tgs).length);
}

/** Talkgroups and radios heard on more than one network. */
export function crossNetwork(tgs: NetTalkgroup[], radios: NetRadio[]): { tgs: NetTalkgroup[]; radios: NetRadio[] } {
  return { tgs: tgs.filter((t) => t.networks.length > 1), radios: radios.filter((r) => r.networks.length > 1) };
}

export interface Ranked { id: string; n: number; }

/** A count map as a list, largest first. */
export function ranked(counts: Record<string, number> | undefined): Ranked[] {
  return keys(counts).map((id) => ({ id, n: counts![id] })).sort((a, b) => b.n - a.n);
}

/** Talkgroups tied to `t` through radios they share (count = shared radios). */
export function linkedTalkgroups(t: NetTalkgroup, ix: NetIndex): Ranked[] {
  const linked: Record<string, number> = {};
  for (const r of keys(t.radios))
    for (const o of keys(ix.rById[r]?.tgs)) if (o !== t.id) linked[o] = (linked[o] || 0) + 1;
  return ranked(linked);
}

/** Radios sharing talkgroups with `r` (count = shared talkgroups). */
export function sharedTalkgroupRadios(r: NetRadio, ix: NetIndex): Ranked[] {
  const co: Record<string, number> = {};
  for (const t of keys(r.tgs))
    for (const o of keys(ix.tgById[t]?.radios)) if (o !== r.id) co[o] = (co[o] || 0) + 1;
  return ranked(co);
}

/** A network's busiest talkgroups and radios. */
export function busiestOn(net: NetNetwork, ix: NetIndex): { tgs: Ranked[]; radios: Ranked[] } {
  return {
    tgs: ix.tgs.filter((t) => t.networks.includes(net.key)).sort((a, b) => b.calls - a.calls).map((t) => ({ id: t.id, n: t.calls })),
    radios: ix.radios.filter((r) => r.networks.includes(net.key)).sort((a, b) => b.calls - a.calls).map((r) => ({ id: r.id, n: r.calls })),
  };
}

/** Recent calls involving a talkgroup (group calls) or a radio (either end). */
export function recentCalls(ix: NetIndex, sel: { type: 'tg' | 'radio'; id: string }, max = 12): NetCall[] {
  const f = sel.type === 'tg'
    ? (c: NetCall) => !c.priv && c.tgt === sel.id
    : (c: NetCall) => c.src === sel.id || (c.priv && c.tgt === sel.id);
  return ix.calls.filter(f).slice(0, max);
}
