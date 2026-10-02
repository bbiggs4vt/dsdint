import { keys } from '../../core/format';
import { NetRadio, NetTalkgroup } from '../../core/models';

// The association graph's model and force layout (a port of the built-in
// page's self-contained engine). Drawing lives in the graph component.

export interface GraphNode {
  id: string;                 // 't:<tg>' or 'r:<radio>'
  kind: 'tg' | 'radio';
  ref: NetTalkgroup | NetRadio;
  x: number; y: number; vx: number; vy: number;
  isNew: boolean;
}
export interface GraphLink { a: string; b: string; w: number; priv: boolean; }
export interface GraphData {
  nodes: GraphNode[];
  links: GraphLink[];
  by: Record<string, GraphNode>;
  adj: Record<string, string[]>;
  /** Candidates before the node cap (for "busiest N of M"). */
  total: number;
  /** Node / link set, to tell a real change from a count bump. */
  structure: string;
}
export interface Transform { k: number; x: number; y: number; }

/**
 * The busiest `cap` talkgroups and radios (talkgroups score calls x2 +
 * radios; radios calls + talkgroups), their radio-talkgroup links and, with
 * `priv`, radio-radio private-call links. Nodes already placed (`old`) keep
 * their position; new ones start next to a placed neighbour. Radios with no
 * link are dropped when the graph is busy (over 30 nodes).
 */
export function buildGraph(tgs: NetTalkgroup[], radios: NetRadio[], cap: number, priv: boolean,
                           old: Record<string, GraphNode> = {}, rnd: () => number = Math.random): GraphData {
  type Cand = { id: string; kind: 'tg' | 'radio'; ref: NetTalkgroup | NetRadio; score: number };
  const cand: Cand[] = [
    ...tgs.map((t): Cand => ({ id: 't:' + t.id, kind: 'tg', ref: t, score: t.calls * 2 + keys(t.radios).length })),
    ...radios.map((r): Cand => ({ id: 'r:' + r.id, kind: 'radio', ref: r, score: r.calls + keys(r.tgs).length })),
  ].sort((a, b) => b.score - a.score);
  const keep: Record<string, Cand> = {};
  for (const c of cand.slice(0, cap)) keep[c.id] = c;

  let links: GraphLink[] = [];
  const deg: Record<string, number> = {};
  const add = (a: string, b: string, w: number, p: boolean) => {
    links.push({ a, b, w, priv: p });
    deg[a] = (deg[a] || 0) + 1;
    deg[b] = (deg[b] || 0) + 1;
  };
  for (const r of radios) {
    const rid = 'r:' + r.id;
    if (!keep[rid]) continue;
    for (const t of keys(r.tgs)) if (keep['t:' + t]) add(rid, 't:' + t, r.tgs[t], false);
    if (priv) for (const p of keys(r.peers)) if (r.id < p && keep['r:' + p]) add(rid, 'r:' + p, r.peers[p], true);
  }
  const by: Record<string, GraphNode> = {}, nodes: GraphNode[] = [];
  const busy = Object.keys(keep).length > 30;
  for (const id of Object.keys(keep)) {
    const c = keep[id];
    if (!deg[id] && c.kind === 'radio' && busy) continue;
    const o = old[id];
    const nd: GraphNode = o || { id, kind: c.kind, ref: c.ref, x: (rnd() - 0.5) * 300, y: (rnd() - 0.5) * 300, vx: 0, vy: 0, isNew: true };
    nd.kind = c.kind;
    nd.ref = c.ref;
    nd.isNew = !o;
    by[id] = nd;
    nodes.push(nd);
  }
  links = links.filter((l) => by[l.a] && by[l.b]);
  const adj: Record<string, string[]> = {};
  for (const l of links) {
    (adj[l.a] ||= []).push(l.b);
    (adj[l.b] ||= []).push(l.a);
  }
  for (const n of nodes) {
    if (!n.isNew) continue;
    const nb = (adj[n.id] || []).map((x) => by[x]).find((x) => x && !x.isNew);
    if (nb) { n.x = nb.x + (rnd() - 0.5) * 40; n.y = nb.y + (rnd() - 0.5) * 40; }
  }
  const structure = nodes.map((x) => x.id).sort().join(',') + '|' + links.map((l) => l.a + '>' + l.b).sort().join(',');
  return { nodes, links, by, adj, total: cand.length, structure };
}

/** Node size: talkgroups grow with their calls more than radios. */
export function nodeRadius(n: { kind: string; ref: { calls: number } }): number {
  return n.kind === 'tg' ? 7 + Math.min(14, Math.sqrt(n.ref.calls) * 2) : 4 + Math.min(6, Math.sqrt(n.ref.calls));
}

/**
 * One step of the force layout at temperature `alpha`: pairwise repulsion
 * (within 400 units), springs along links (shorter for private calls), a
 * pull to the centre, damping. `pinned` (a dragged node) doesn't move.
 * Returns the cooled alpha.
 */
export function layoutStep(g: Pick<GraphData, 'nodes' | 'links' | 'by'>, alpha: number, pinned: GraphNode | null = null,
                           rnd: () => number = Math.random): number {
  const N = g.nodes, n = N.length;
  for (let i = 0; i < n; i++) {
    const A = N[i];
    for (let j = i + 1; j < n; j++) {
      const B = N[j];
      let dx = B.x - A.x, dy = B.y - A.y, d2 = dx * dx + dy * dy;
      if (d2 > 160000) continue;
      if (d2 < 1) { dx = rnd() - 0.5; dy = rnd() - 0.5; d2 = 1; }
      const d = Math.sqrt(d2), f = 2600 * alpha / d2, ux = dx / d * f, uy = dy / d * f;
      A.vx -= ux; A.vy -= uy; B.vx += ux; B.vy += uy;
    }
  }
  for (const l of g.links) {
    const A = g.by[l.a], B = g.by[l.b], dx = B.x - A.x, dy = B.y - A.y, d = Math.sqrt(dx * dx + dy * dy) || 1;
    const rest = l.priv ? 55 : 75, f = (d - rest) * 0.05 * alpha * Math.min(2, 0.6 + Math.log2(1 + l.w) * 0.3);
    const ux = dx / d * f, uy = dy / d * f;
    A.vx += ux; A.vy += uy; B.vx -= ux; B.vy -= uy;
  }
  for (const q of N) {
    q.vx -= q.x * 0.008 * alpha;
    q.vy -= q.y * 0.008 * alpha;
    if (q === pinned) { q.vx = q.vy = 0; continue; }
    q.vx *= 0.55; q.vy *= 0.55;
    q.x += Math.max(-30, Math.min(30, q.vx));
    q.y += Math.max(-30, Math.min(30, q.vy));
  }
  return alpha * 0.985;
}

/** The transform that shows every node in a w x h view (zoom capped so a small graph isn't blown up). */
export function fitTransform(nodes: GraphNode[], w: number, h: number): Transform {
  if (!nodes.length) return { k: 1, x: 0, y: 0 };
  let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const n of nodes) { x0 = Math.min(x0, n.x); y0 = Math.min(y0, n.y); x1 = Math.max(x1, n.x); y1 = Math.max(y1, n.y); }
  const k = Math.min(1.35, Math.max(0.15, Math.min(w / (x1 - x0 + 160), h / (y1 - y0 + 100))));
  return { k, x: -(x0 + x1) / 2 * k, y: -(y0 + y1) / 2 * k };
}

/** Zoom to scale k1 (0.1-6) keeping the point (cx, cy) -- relative to the view's centre -- in place. */
export function zoomAt(t: Transform, cx: number, cy: number, k1: number): Transform {
  k1 = Math.max(0.1, Math.min(6, k1));
  return { k: k1, x: cx - (cx - t.x) * k1 / t.k, y: cy - (cy - t.y) * k1 / t.k };
}
