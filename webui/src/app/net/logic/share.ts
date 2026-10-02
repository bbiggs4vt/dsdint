import { NetDoc, NetFamily } from '../../core/models';
import { callKey } from './calls';

/** Same content: primitives by value, arrays / objects by their JSON. */
function same(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (typeof a !== 'object' || typeof b !== 'object' || !a || !b) return false;
  const ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) {
    const x = (a as Record<string, unknown>)[k], y = (b as Record<string, unknown>)[k];
    if (x === y) continue;
    if (typeof x !== 'object' || typeof y !== 'object' || JSON.stringify(x) !== JSON.stringify(y)) return false;
  }
  return true;
}

/** `next` with every element equal to one in `prev` (by key) replaced by that one. */
export function reuse<T>(prev: T[] | undefined, next: T[], key: (x: T) => string): T[] {
  if (!prev || !prev.length) return next;
  const old = new Map<string, T>();
  for (const x of prev) old.set(key(x), x);
  let changed = prev.length !== next.length;
  const out = next.map((x, i) => {
    const o = old.get(key(x));
    const r = o !== undefined && same(o, x) ? o : x;
    if (r !== prev[i]) changed = true;
    return r;
  });
  return changed ? out : prev;
}

/**
 * A new /net.json with the previous poll's objects kept wherever nothing
 * changed, so the views only re-render what did (each poll otherwise rebuilds
 * thousands of calls, radios and talkgroups).
 */
export function shareUnchanged(prev: NetDoc | null, next: NetDoc): NetDoc {
  if (!prev) return next;
  const families: Record<string, NetFamily> = {};
  for (const f of Object.keys(next.families)) {
    const a = prev.families[f], b = next.families[f];
    if (!a) { families[f] = b; continue; }
    const fam: NetFamily = {
      networks: reuse(a.networks, b.networks, (n) => n.key),
      talkgroups: reuse(a.talkgroups, b.talkgroups, (t) => t.id),
      radios: reuse(a.radios, b.radios, (r) => r.id),
      calls: reuse(a.calls, b.calls, callKey),
    };
    families[f] = fam.networks === a.networks && fam.talkgroups === a.talkgroups && fam.radios === a.radios && fam.calls === a.calls ? a : fam;
  }
  return { ...next, families };
}
