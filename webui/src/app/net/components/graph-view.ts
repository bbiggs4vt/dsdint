import { Component, DestroyRef, ElementRef, afterNextRender, computed, effect, inject, signal, untracked, viewChild, ChangeDetectionStrategy } from '@angular/core';
import { GraphData, GraphNode, Transform, buildGraph, fitTransform, layoutStep, nodeRadius, zoomAt } from '../logic/graph';
import { primaryNet } from '../logic/model-index';
import { NetRadio } from '../../core/models';
import { Breakpoints } from '../services/breakpoints';
import { NetStore } from '../state/net-store';
import { Swatch } from './widgets';

const SVGNS = 'http://www.w3.org/2000/svg';
function sv(tag: string, attrs: Record<string, string | number>): SVGElement {
  const e = document.createElementNS(SVGNS, tag) as SVGElement;
  for (const k of Object.keys(attrs)) e.setAttribute(k, String(attrs[k]));
  return e;
}
type DrawnNode = GraphNode & { el?: SVGElement };
type DrawnLink = GraphData['links'][number] & { el?: SVGElement };

/**
 * The association graph: talkgroups (squares) and radios (dots), radio-
 * talkgroup links and dashed private-call links, laid out by a force
 * simulation; drag nodes, pan, zoom (wheel, pinch, buttons), click to select.
 */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-graph-view',
  imports: [Swatch],
  templateUrl: './graph-view.html',
})
export class GraphView {
  private readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  private readonly svgRef = viewChild.required<ElementRef<SVGSVGElement>>('svg');

  readonly cap = signal(250);
  readonly priv = signal(true);
  readonly labels = signal(true);
  readonly note = signal('');
  readonly nets = computed(() => this.store.ix()?.nets ?? []);

  /** Rebuilt when the data, protocol, filter or graph options change. */
  private readonly data = computed(() => ({
    version: this.store.doc()?.version, fam: this.store.activeFam(), tgs: this.store.tgs(), radios: this.store.radios(),
    cap: this.cap(), priv: this.priv(),
  }));

  private g: GraphData = { nodes: [], links: [], by: {}, adj: {}, total: 0, structure: '' };
  private alpha = 0;
  private raf = 0;
  private root: SVGElement | null = null;
  private t: Transform;
  private drag: { n: DrawnNode; moved: boolean; sx: number; sy: number } | null = null;
  private pan: { x: number; y: number; tx: number; ty: number; moved: boolean } | null = null;
  private pts = new Map<number, { x: number; y: number }>();
  private pinch: { d0: number; m0: { x: number; y: number }; k0: number; x0: number; y0: number } | null = null;
  private ready = false;
  private destroyed = false;

  constructor() {
    this.t = { ...this.store.graphCache.t };
    afterNextRender(() => {
      this.ready = true;
      this.wire();
      this.rebuild();
    });
    effect(() => {
      this.data();
      if (this.ready) untracked(() => this.rebuild());
    });
    effect(() => {
      this.labels();
      if (this.ready) untracked(() => this.draw());
    });
    effect(() => {
      this.store.sel();
      if (this.ready) untracked(() => this.highlight());
    });
    inject(DestroyRef).onDestroy(() => {
      this.destroyed = true;
      cancelAnimationFrame(this.raf);
      this.store.graphCache.t = this.t;
    });
  }

  private get svg(): SVGSVGElement { return this.svgRef().nativeElement; }

  /** Node count, for tests and the note. */
  get nodes(): GraphNode[] { return this.g.nodes; }

  rebuild(): void {
    const d = this.data(), cache = this.store.graphCache;
    const famChanged = cache.fam !== d.fam;
    const old = famChanged ? {} : (cache.by as Record<string, GraphNode>);
    const prev = this.g.structure;
    this.g = buildGraph(d.tgs, d.radios, d.cap, d.priv, old);
    cache.by = this.g.by;
    if (famChanged) { cache.fam = d.fam; cache.fitted = false; }
    this.note.set(this.g.nodes.length + ' nodes · ' + this.g.links.length + ' links' +
      (this.g.total > d.cap ? ' (busiest ' + d.cap + ' of ' + this.g.total + ')' : ''));
    this.draw();
    // Only re-heat the layout when the node / link set changed (every voice
    // frame bumps the data version).
    if (this.g.structure !== prev || famChanged) this.alpha = Math.max(this.alpha, famChanged || !cache.fitted ? 1 : 0.35);
    this.run();
  }

  private color(n: GraphNode): string {
    const ix = this.store.ix(), k = ix ? primaryNet(ix, n.ref.networks) : null;
    return this.store.colors.colorFor(this.store.activeFam() || '', k && ix ? ix.netByKey[k] : undefined);
  }

  draw(): void {
    const svg = this.svg;
    svg.textContent = '';
    this.root = sv('g', {});
    const lg = sv('g', {}), ng = sv('g', {});
    this.root.append(lg, ng);
    svg.appendChild(this.root);
    for (const l of this.g.links as DrawnLink[]) {
      l.el = sv('line', { class: 'glink' + (l.priv ? ' priv' : ''), 'stroke-width': Math.min(5, 1 + Math.log2(1 + l.w)) });
      lg.appendChild(l.el);
    }
    for (const n of this.g.nodes as DrawnNode[]) {
      const g = sv('g', { class: 'gnode ' + n.kind + (n.ref.networks.length > 1 ? ' multi' : '') });
      g.setAttribute('data-id', n.id);
      const r = nodeRadius(n), col = this.color(n);
      g.appendChild(n.kind === 'tg'
        ? sv('rect', { class: 'shape', x: -r, y: -r, width: 2 * r, height: 2 * r, rx: 3, fill: col })
        : sv('circle', { class: 'shape', r, fill: col }));
      const radio = n.kind === 'radio' ? (n.ref as NetRadio) : null;
      const title = document.createElementNS(SVGNS, 'title');
      title.textContent = (n.kind === 'tg' ? 'Talkgroup ' : 'Radio ') + n.ref.id +
        (radio && radio.aliases.length ? ' (' + radio.aliases.join(' / ') + ')' : '') + ' — ' + n.ref.calls + ' calls';
      g.appendChild(title);
      if (n.kind === 'tg' || this.labels()) {
        const tx = sv('text', { x: r + 3, y: 3.5 });
        tx.textContent = n.kind === 'tg' ? 'TG ' + n.ref.id : n.ref.id + (radio && radio.aliases.length ? ' ' + radio.aliases[radio.aliases.length - 1] : '');
        g.appendChild(tx);
      }
      g.addEventListener('pointerdown', (e: PointerEvent) => {
        if (!this.pinch && !this.pts.size) this.drag = { n, moved: false, sx: e.clientX, sy: e.clientY };
      });
      n.el = g;
      ng.appendChild(g);
    }
    this.highlight();
    this.paint();
  }

  highlight(): void {
    const sel = this.store.sel();
    const id = sel && sel.type !== 'net' ? (sel.type === 'tg' ? 't:' : 'r:') + sel.id : null;
    const focus = !!id && !!this.g.by[id];
    this.svg.classList.toggle('focus', focus);
    const nb = new Set<string>(focus ? [id!, ...(this.g.adj[id!] || [])] : []);
    for (const n of this.g.nodes as DrawnNode[]) {
      n.el?.classList.toggle('hl', nb.has(n.id));
      n.el?.classList.toggle('sel', n.id === id);
    }
    for (const l of this.g.links as DrawnLink[]) l.el?.classList.toggle('hl', focus && (l.a === id || l.b === id));
  }

  paint(): void {
    const w = this.svg.clientWidth || 800, h = this.svg.clientHeight || 600;
    this.root?.setAttribute('transform', 'translate(' + (w / 2 + this.t.x) + ',' + (h / 2 + this.t.y) + ') scale(' + this.t.k + ')');
    for (const l of this.g.links as DrawnLink[]) {
      const a = this.g.by[l.a], b = this.g.by[l.b];
      l.el?.setAttribute('x1', a.x.toFixed(1)); l.el?.setAttribute('y1', a.y.toFixed(1));
      l.el?.setAttribute('x2', b.x.toFixed(1)); l.el?.setAttribute('y2', b.y.toFixed(1));
    }
    for (const n of this.g.nodes as DrawnNode[]) n.el?.setAttribute('transform', 'translate(' + n.x.toFixed(1) + ',' + n.y.toFixed(1) + ')');
  }

  private run(): void {
    if (this.raf || this.destroyed) return;
    const frame = () => {
      const cache = this.store.graphCache;
      if (this.destroyed || this.alpha < 0.012) {
        this.raf = 0;
        if (!this.destroyed && !cache.fitted && this.g.nodes.length) { this.fit(); cache.fitted = true; }
        return;
      }
      for (let k = 0; k < 2; k++) this.alpha = layoutStep(this.g, this.alpha, this.drag?.n ?? null);
      if (!cache.fitted && this.alpha < 0.2 && this.g.nodes.length) { this.fit(); cache.fitted = true; }
      this.paint();
      this.raf = requestAnimationFrame(frame);
    };
    frame();
  }

  fit(): void {
    this.t = fitTransform(this.g.nodes, this.svg.clientWidth || 800, this.svg.clientHeight || 600);
    this.paint();
  }
  zoom(f: number): void {
    this.t = zoomAt(this.t, 0, 0, this.t.k * f);
    this.paint();
  }

  /** Pointer handling: pan the background, drag nodes, pinch-zoom, wheel-zoom; a tap selects / deselects. */
  private wire(): void {
    const svg = this.svg;
    const centred = (x: number, y: number) => {
      const r = svg.getBoundingClientRect();
      return { x: x - r.left - r.width / 2, y: y - r.top - r.height / 2 };
    };
    const toGraph = (x: number, y: number) => {
      const c = centred(x, y);
      return { x: (c.x - this.t.x) / this.t.k, y: (c.y - this.t.y) / this.t.k };
    };
    const two = () => {
      const [a, b] = [...this.pts.values()];
      return { d: Math.max(1, Math.hypot(a.x - b.x, a.y - b.y)), m: centred((a.x + b.x) / 2, (a.y + b.y) / 2) };
    };
    svg.addEventListener('pointerdown', (e) => {
      this.pts.set(e.pointerId, { x: e.clientX, y: e.clientY });
      try { svg.setPointerCapture(e.pointerId); } catch { /* jsdom */ }
      if (this.pts.size === 2) {
        const g = two();
        this.pinch = { d0: g.d, m0: g.m, k0: this.t.k, x0: this.t.x, y0: this.t.y };
        this.drag = null;
        this.pan = null;
        svg.classList.remove('panning');
        return;
      }
      if (this.drag || this.pinch) return;
      this.pan = { x: e.clientX, y: e.clientY, tx: this.t.x, ty: this.t.y, moved: false };
      svg.classList.add('panning');
    });
    svg.addEventListener('pointermove', (e) => {
      if (this.pts.has(e.pointerId)) this.pts.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (this.pinch) {
        if (this.pts.size < 2) return;
        const g = two(), P = this.pinch, k1 = Math.max(0.1, Math.min(6, P.k0 * g.d / P.d0));
        this.t = { k: k1, x: g.m.x - (P.m0.x - P.x0) * k1 / P.k0, y: g.m.y - (P.m0.y - P.y0) * k1 / P.k0 };
        this.paint();
      } else if (this.drag) {
        const dg = this.drag;
        if (Math.abs(e.clientX - dg.sx) + Math.abs(e.clientY - dg.sy) > 3) dg.moved = true;
        if (!dg.moved) return;
        const p = toGraph(e.clientX, e.clientY);
        dg.n.x = p.x;
        dg.n.y = p.y;
        this.alpha = Math.max(this.alpha, 0.25);
        this.run();
        this.paint();
      } else if (this.pan) {
        const dx = e.clientX - this.pan.x, dy = e.clientY - this.pan.y;
        if (Math.abs(dx) + Math.abs(dy) > 3) this.pan.moved = true;
        this.t = { ...this.t, x: this.pan.tx + dx, y: this.pan.ty + dy };
        this.paint();
      }
    });
    const up = (e: PointerEvent) => {
      this.pts.delete(e.pointerId);
      if (this.pinch) {
        this.pinch = null;
        if (this.pts.size === 1) {
          const q = [...this.pts.values()][0];
          this.pan = { x: q.x, y: q.y, tx: this.t.x, ty: this.t.y, moved: true };
        }
        return;
      }
      if (this.drag) {
        const dg = this.drag;
        this.drag = null;
        if (!dg.moved) this.store.select(dg.n.kind === 'tg' ? 'tg' : 'radio', dg.n.ref.id, this.bp.drawer());
      } else if (this.pan) {
        if (!this.pan.moved && this.store.sel()) this.store.closeSheet();
        this.pan = null;
      }
      svg.classList.remove('panning');
    };
    svg.addEventListener('pointerup', up);
    svg.addEventListener('pointercancel', up);
    svg.addEventListener('wheel', (e) => {
      e.preventDefault();
      const c = centred(e.clientX, e.clientY);
      this.t = zoomAt(this.t, c.x, c.y, this.t.k * Math.exp(-e.deltaY * 0.0015));
      this.paint();
    }, { passive: false });
  }
}
