import { Injectable, computed, signal } from '@angular/core';
import { NetCall, NetDoc, NetFamily } from '../../core/models';
import { store } from '../../core/storage';
import { callKey } from '../logic/calls';
import { shareUnchanged } from '../logic/share';
import { ALL, ViewFilter, filterCalls, filterRadios, filterTalkgroups } from '../logic/filters';
import { NetColors, NetIndex, buildIndex } from '../logic/model-index';

export type ViewName = 'calls' | 'tgs' | 'radios' | 'graph' | 'links' | 'nets';
export const VIEWS: ViewName[] = ['calls', 'tgs', 'radios', 'graph', 'links', 'nets'];
export type SelType = 'tg' | 'radio' | 'net';
export interface Selection { type: SelType; id: string; }
export interface SortState { i: number; dir: number; }
export interface FileView { name: string; exported: number; source: string; }
/** Pause list: the calls listed when it was pressed. */
export interface Hold { fam: string; list: NetCall[]; keys: Set<string>; }

export const FAMILY_NAMES: Record<string, string> = {
  dmr: 'DMR', p25: 'P25', nxdn: 'NXDN', tetra: 'TETRA', dpmr: 'dPMR', dstar: 'D-STAR', ysf: 'YSF',
  edacs: 'EDACS / ProVoice', x2tdma: 'X2-TDMA',
};

function totals(F: NetFamily): number {
  return F.calls.length + F.talkgroups.length + F.radios.length;
}

/** The explorer's state: what is shown, filtered and selected. Views read it; actions change it. */
@Injectable()
export class NetStore {
  readonly doc = signal<NetDoc | null>(null);
  /** Viewing a saved export (read-only), not the live server. */
  readonly file = signal<FileView | null>(null);
  readonly fam = signal<string | null>(store.get('fam'));
  readonly net = signal<string>(ALL);
  readonly view = signal<ViewName>((VIEWS as string[]).includes(store.get('view') || '') ? (store.get('view') as ViewName) : 'calls');
  readonly sel = signal<Selection | null>(null);
  readonly q = signal('');
  /** Header Pause: the whole page stops updating. */
  readonly paused = signal(false);
  readonly hold = signal<Hold | null>(null);
  readonly audioOnly = signal(store.get('audonly') === '1');
  readonly sort = signal<Record<string, SortState | null>>({});
  /** The details drawer is open (narrow screens). */
  readonly sheet = signal(false);
  /** Server time minus ours, from the last poll. */
  readonly skew = signal(0);
  /** "Now" for ages and live durations -- moves on with every poll. */
  readonly now = signal(Date.now());
  readonly colors = new NetColors();
  /** The graph's layout, kept while switching views (positions, zoom). */
  graphCache: { fam: string | null; by: Record<string, unknown>; t: { k: number; x: number; y: number }; fitted: boolean } =
    { fam: null, by: {}, t: { k: 1, x: 0, y: 0 }, fitted: false };

  /** Protocol families with data, busiest first. */
  readonly families = computed(() => {
    const fams = this.doc()?.families || {};
    return Object.keys(fams).sort((a, b) => totals(fams[b]) - totals(fams[a]));
  });
  /** The protocol shown (the remembered one if it has data, else the busiest). */
  readonly activeFam = computed(() => {
    const f = this.fam(), list = this.families();
    return f && list.includes(f) ? f : (list[0] ?? null);
  });
  readonly ix = computed<NetIndex | null>(() => {
    const d = this.doc(), f = this.activeFam();
    if (!d || !f) return null;
    const ix = buildIndex(d.families[f]);
    this.colors.assign(f, ix);
    return ix;
  });
  /** The network filter, if that network still exists. */
  readonly activeNet = computed(() => {
    const n = this.net(), ix = this.ix();
    return n !== ALL && ix && !ix.netByKey[n] ? ALL : n;
  });
  readonly filter = computed<ViewFilter>(() => ({ net: this.activeNet(), q: this.q(), audioOnly: this.audioOnly() && !this.file() }));
  readonly calls = computed(() => (this.ix() ? filterCalls(this.ix()!, this.filter()) : []));
  readonly tgs = computed(() => (this.ix() ? filterTalkgroups(this.ix()!, this.filter()) : []));
  readonly radios = computed(() => (this.ix() ? filterRadios(this.ix()!, this.filter()) : []));
  readonly fileMode = computed(() => !!this.file());

  setFam(f: string): void {
    this.fam.set(f);
    store.set('fam', f);
    this.net.set(ALL);
    this.sel.set(null);
  }
  setView(v: ViewName): void {
    this.view.set(v);
    store.set('view', v);
  }
  setNet(k: string): void {
    this.net.set(k);
  }
  setAudioOnly(on: boolean): void {
    this.audioOnly.set(on);
    store.set('audonly', on ? '1' : '0');
  }
  isSel(type: SelType, id: string): boolean {
    const s = this.sel();
    return !!s && s.type === type && s.id === id;
  }
  /**
   * Select (or deselect) a radio, talkgroup or network. On a narrow screen the
   * details are a drawer: tapping the selected item again while the drawer is
   * closed reopens it rather than deselecting.
   */
  select(type: SelType, id: string, drawer: boolean): void {
    const reopen = drawer && this.isSel(type, id) && !this.sheet();
    this.sel.set(this.isSel(type, id) && !reopen ? null : { type, id });
    this.sheet.set(!!this.sel() && drawer);
  }
  closeSheet(): void {
    this.sheet.set(false);
    this.sel.set(null);
  }
  /** Pause list on / off: holds the calls listed now (of the protocol shown). */
  setHold(on: boolean): void {
    const ix = this.ix(), fam = this.activeFam();
    if (!on || !ix || !fam) { this.hold.set(null); return; }
    const list = ix.calls.slice();
    this.hold.set({ fam, list, keys: new Set(list.map(callKey)) });
  }
  /** A new /net.json (or an opened file). */
  setDoc(d: NetDoc, now = Date.now()): void {
    if (!this.file()) this.skew.set(d.now - now);
    this.doc.set(this.file() ? d : shareUnchanged(this.doc(), d));
    this.now.set(this.file() ? d.now : now + this.skew());
  }
  tick(now = Date.now()): void {
    const d = this.doc();
    this.now.set(this.file() && d ? d.now : now + this.skew());
  }
}
