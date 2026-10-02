import { buildIndex } from './model-index';
import { family } from './fixture.spec-helper';
import { buildGraph, fitTransform, layoutStep, nodeRadius, zoomAt } from './graph';

function seeded(seed = 1) {
  return () => ((seed = (seed * 16807) % 2147483647) - 1) / 2147483646;
}

describe('graph', () => {
  const ix = buildIndex(family());

  it('builds nodes and links from talkgroups, radios and private calls', () => {
    const g = buildGraph(ix.tgs, ix.radios, 250, true, {}, seeded());
    expect(g.nodes.map((n) => n.id).sort()).toEqual(['r:100', 'r:200', 'r:300', 'r:400', 't:10', 't:9']);
    expect(g.links.filter((l) => !l.priv).length).toBe(4);
    const p = g.links.find((l) => l.priv)!;
    expect([p.a, p.b]).toEqual(['r:300', 'r:400']);
    expect(p.w).toBe(2);
    expect(g.adj['t:9'].sort()).toEqual(['r:100', 'r:200']);
    expect(g.total).toBe(6);
  });

  it('leaves out private links when asked, and caps the nodes', () => {
    expect(buildGraph(ix.tgs, ix.radios, 250, false, {}, seeded()).links.some((l) => l.priv)).toBe(false);
    const g = buildGraph(ix.tgs, ix.radios, 2, true, {}, seeded());
    expect(g.nodes.map((n) => n.id).sort()).toEqual(['t:10', 't:9']);   // talkgroups score highest
    expect(g.links).toEqual([]);
  });

  it('keeps placed nodes where they are and seeds new ones next to a neighbour', () => {
    const g1 = buildGraph(ix.tgs, ix.radios, 250, true, {}, seeded());
    g1.by['t:9'].x = 500; g1.by['t:9'].y = 500;
    const old = { 't:9': g1.by['t:9'] };
    const g2 = buildGraph(ix.tgs, ix.radios, 250, true, old, seeded(7));
    expect(g2.by['t:9']).toBe(old['t:9']);
    expect(g2.by['t:9'].isNew).toBe(false);
    expect(Math.abs(g2.by['r:100'].x - 500)).toBeLessThanOrEqual(20);   // next to TG 9
  });

  it('notices only real structure changes', () => {
    const a = buildGraph(ix.tgs, ix.radios, 250, true, {}, seeded());
    const F = family();
    F.talkgroups[0].calls = 99;                           // a count bump
    const ix2 = buildIndex(F);
    expect(buildGraph(ix2.tgs, ix2.radios, 250, true, {}, seeded()).structure).toBe(a.structure);
    expect(buildGraph(ix.tgs, ix.radios, 250, false, {}, seeded()).structure).not.toBe(a.structure);
  });

  it('sizes nodes by calls', () => {
    expect(nodeRadius({ kind: 'tg', ref: { calls: 0 } })).toBe(7);
    expect(nodeRadius({ kind: 'tg', ref: { calls: 100 } })).toBe(21);
    expect(nodeRadius({ kind: 'radio', ref: { calls: 4 } })).toBe(6);
  });

  it('settles: linked nodes end near their rest length, alpha cools', () => {
    const g = buildGraph(ix.tgs, ix.radios, 250, true, {}, seeded());
    let alpha = 1;
    for (let i = 0; i < 400; i++) alpha = layoutStep(g, alpha, null, seeded(i + 2));
    expect(alpha).toBeLessThan(0.01);
    const d = (a: string, b: string) => Math.hypot(g.by[a].x - g.by[b].x, g.by[a].y - g.by[b].y);
    expect(d('r:300', 'r:400')).toBeGreaterThan(20);
    expect(d('r:300', 'r:400')).toBeLessThan(200);
    for (const n of g.nodes) expect(Number.isFinite(n.x) && Number.isFinite(n.y)).toBe(true);
  });

  it('never moves a pinned (dragged) node', () => {
    const g = buildGraph(ix.tgs, ix.radios, 250, true, {}, seeded());
    const n = g.by['t:9'], x = n.x, y = n.y;
    layoutStep(g, 1, n, seeded());
    expect([n.x, n.y]).toEqual([x, y]);
  });

  it('fits and zooms', () => {
    const nodes = [{ x: -100, y: -50 }, { x: 100, y: 50 }] as never[];
    const t = fitTransform(nodes, 800, 600);
    expect(t.k).toBeCloseTo(Math.min(1.35, 800 / 360, 600 / 200));
    expect(t.x).toBeCloseTo(0);
    expect(fitTransform([], 800, 600)).toEqual({ k: 1, x: 0, y: 0 });
    const z = zoomAt({ k: 1, x: 0, y: 0 }, 100, 0, 2);
    expect(z).toEqual({ k: 2, x: -100, y: 0 });           // the point under the cursor stays put
    expect(zoomAt({ k: 1, x: 0, y: 0 }, 0, 0, 99).k).toBe(6);
  });
});
