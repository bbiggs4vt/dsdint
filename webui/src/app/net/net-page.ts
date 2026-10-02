import { Component, DestroyRef, DOCUMENT, computed, effect, inject, signal, ChangeDetectionStrategy } from '@angular/core';
import { RouterLink } from '@angular/router';
import { firstValueFrom } from 'rxjs';
import { dt, hms, mb } from '../core/format';
import { ImportInfo, ImportResult, MergeReportItem, NetDoc, NetExport } from '../core/models';
import { NetApi } from '../core/net-api.service';
import { store } from '../core/storage';
import { callRate, isLive } from './logic/calls';
import { ALL } from './logic/filters';
import { exportProblem, isRecording, sourceText } from './logic/export-file';
import { AsrService } from './services/asr';
import { Breakpoints } from './services/breakpoints';
import { Player } from './services/player';
import { Toast } from './services/toast';
import { FAMILY_NAMES, NetStore, VIEWS, ViewName } from './state/net-store';
import { CallsView } from './components/calls-view';
import { DetailsPanel } from './components/details-panel';
import { LinksView, NetworksView, RadiosView, TalkgroupsView } from './components/entity-views';
import { GraphView } from './components/graph-view';
import { NowPlaying } from './components/now-playing';
import { Swatch } from './components/widgets';

export const POLL_MS = 1500;
const VIEW_LABELS: Record<ViewName, string> = {
  calls: 'Calls', tgs: 'Talkgroups', radios: 'Radios', graph: 'Graph', links: 'Links', nets: 'Networks',
};
/** Reads a dropped / chosen file, inflating .gz where the browser can. */
export function readFile(f: File): Promise<string> {
  const gz = /\.gz$/i.test(f.name) && typeof DecompressionStream !== 'undefined';
  return gz ? new Response(f.stream().pipeThrough(new DecompressionStream('gzip'))).text() : f.text();
}

/**
 * The network explorer (port of the built-in /net page): calls, talkgroups,
 * radios and networks, and the associations between them, per protocol; live
 * from /net.json, or from saved exports.
 */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-net-page',
  imports: [RouterLink, CallsView, TalkgroupsView, RadiosView, NetworksView, LinksView, GraphView, DetailsPanel, NowPlaying, Swatch],
  providers: [NetStore, Player, AsrService, Toast],
  templateUrl: './net-page.html',
})
export class NetPage {
  protected readonly store = inject(NetStore);
  protected readonly bp = inject(Breakpoints);
  protected readonly player = inject(Player);
  protected readonly toast = inject(Toast);
  private readonly api = inject(NetApi);
  private readonly document = inject(DOCUMENT);

  protected readonly VIEWS = VIEWS;
  protected readonly VIEW_LABELS = VIEW_LABELS;
  protected readonly FAMILY_NAMES = FAMILY_NAMES;
  protected readonly ALL = ALL;
  protected readonly mb = mb;

  /** "connecting…", "live · updated …", "disconnected — retrying". */
  readonly status = signal('connecting…');
  readonly menuOpen = signal(false);
  readonly exportOpen = signal(false);
  readonly dragging = signal(false);
  /** Results of the last Import. */
  readonly importNote = signal<{ busy: boolean; results: ImportResult[] } | null>(null);
  /** The merge report of an opened multi-file view, and its file for "Save merged". */
  readonly fileReport = signal<{ report: MergeReportItem[] | null; sources: string[]; savedUrl: string | null } | null>(null);
  /** Poll interval; tests stretch it to drive poll() themselves. */
  pollMs = POLL_MS;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private stopped = false;
  private qTimer: ReturnType<typeof setTimeout> | undefined;

  readonly doc = this.store.doc;
  readonly rec = computed(() => this.doc()?.rec || {});
  readonly audio = computed(() => this.doc()?.audio || {});
  readonly imports = computed<ImportInfo[]>(() => (this.store.fileMode() ? [] : this.doc()?.imports || []));
  readonly liveLabel = computed(() => (this.store.fileMode() ? 'file view' : this.store.paused() ? 'paused — view frozen' : this.status()));

  readonly famTabs = computed(() => {
    const d = this.doc(), now = this.store.now(), file = this.store.fileMode();
    return this.store.families().map((f) => {
      const F = d!.families[f], lv = F.calls.filter((c) => isLive(c, now, file)).length;
      return { f, name: FAMILY_NAMES[f] || f.toUpperCase(), count: F.radios.length + ' radios' + (lv ? ' · ' + lv + ' live' : '') };
    });
  });
  readonly cards = computed(() => {
    const ix = this.store.ix();
    if (!ix) return [];
    const net = this.store.activeNet(), calls = this.store.calls(), now = this.store.now(), file = this.store.fileMode();
    const sites = new Set<string>();
    for (const n of ix.nets) if (net === ALL || n.key === net) for (const s of n.sites) sites.add(n.key + s);
    const lv = calls.filter((c) => isLive(c, now, file)).length;
    const filtered = net !== ALL || !!this.store.q() || file;
    const rate = callRate(this.doc()?.rates?.[this.store.activeFam()!], filtered, calls, now, net !== ALL, !!this.store.q());
    const kept = 'The newest calls are listed (up to ' + (this.doc()?.max_calls || 5000) + ' per protocol; calls with audio are kept longest).';
    return [
      { l: 'Networks', n: net === ALL ? ix.nets.length : 1 }, { l: 'Sites', n: sites.size },
      { l: 'Talkgroups', n: this.store.tgs().length }, { l: 'Radios', n: this.store.radios().length },
      { l: 'Calls (recent)', n: calls.length, tip: kept }, { l: 'Calls / s', n: rate.text, tip: rate.tip },
      { l: 'Live calls', n: lv, live: lv > 0 },
    ];
  });
  readonly chips = computed(() => (this.store.ix()?.nets || []).slice().sort((a, b) => b.calls - a.calls));
  readonly counts = computed<Partial<Record<ViewName, number>>>(() => ({
    calls: this.store.calls().length, tgs: this.store.tgs().length, radios: this.store.radios().length, nets: this.store.ix()?.nets.length,
  }));

  constructor() {
    const destroy = inject(DestroyRef);
    destroy.onDestroy(() => {
      this.stopped = true;
      clearTimeout(this.timer);
      for (const c of ['filemode', 'dragging', 'sheet', 'np-on']) this.document.body.classList.remove(c);
    });
    // Page-level state classes on <body> (the stylesheet keys on them).
    effect(() => {
      const b = this.document.body.classList;
      b.toggle('filemode', this.store.fileMode());
      b.toggle('dragging', this.dragging());
      b.toggle('sheet', this.store.sheet() && this.bp.drawer());
      b.toggle('np-on', !!this.player.call());
    });
    // Leaving the narrow layout closes the drawer.
    effect(() => { if (!this.bp.drawer() && this.store.sheet()) this.store.sheet.set(false); });
    this.wireDocument(destroy);
    queueMicrotask(() => void this.poll());
  }

  /** One poll of /net.json (skipped while paused, in a file view or in a hidden tab). */
  async poll(): Promise<void> {
    if (!this.store.paused() && !this.store.fileMode() && !this.document.hidden) {
      try {
        const d = await firstValueFrom(this.api.net());
        if (!this.store.fileMode()) {
          this.store.setDoc(d);
          this.status.set('live · updated ' + hms(d.now) + 'Z');
        }
      } catch {
        this.status.set('disconnected — retrying');
      }
    }
    if (!this.stopped) this.timer = setTimeout(() => void this.poll(), this.pollMs);
  }

  private wireDocument(destroy: DestroyRef): void {
    const doc = this.document;
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape' && this.store.sheet()) this.store.closeSheet(); };
    // Hover-only explanations (title / data-tip) show on tap on touch screens.
    const onClick = (e: MouseEvent) => {
      const el = (e.target as Element | null)?.closest?.('[data-tip]');
      if (el && this.bp.coarse()) this.toast.show(el.getAttribute('data-tip') || '');
      if (!(e.target as Element | null)?.closest?.('.acts, #more')) this.menuOpen.set(false);
      if (!(e.target as Element | null)?.closest?.('.dropdown')) this.exportOpen.set(false);
    };
    const onOver = (e: DragEvent) => { e.preventDefault(); this.dragging.set(true); };
    const onLeave = (e: DragEvent) => { if (!e.relatedTarget) this.dragging.set(false); };
    const onDrop = (e: DragEvent) => {
      e.preventDefault();
      this.dragging.set(false);
      if (e.dataTransfer?.files.length) void this.openFiles(Array.from(e.dataTransfer.files));
    };
    doc.addEventListener('keydown', onKey);
    doc.addEventListener('click', onClick);
    doc.addEventListener('dragover', onOver);
    doc.addEventListener('dragleave', onLeave);
    doc.addEventListener('drop', onDrop);
    destroy.onDestroy(() => {
      doc.removeEventListener('keydown', onKey);
      doc.removeEventListener('click', onClick);
      doc.removeEventListener('dragover', onOver);
      doc.removeEventListener('dragleave', onLeave);
      doc.removeEventListener('drop', onDrop);
    });
  }

  // ---- header actions ----
  search(q: string): void {
    clearTimeout(this.qTimer);
    this.qTimer = setTimeout(() => this.store.q.set(q.trim()), 200);
  }
  togglePause(): void {
    this.store.paused.update((p) => !p);
  }
  async toggleRecording(): Promise<void> {
    let on = true, clear = false;
    if (this.rec().on) on = false;
    else if (Object.keys(this.doc()?.families || {}).length)
      clear = confirm('Clear the explorer first so the recording replays exactly?\n\n' +
        'OK = clear, then record (recommended)\nCancel = record on top of the current data');
    try {
      const r = await firstValueFrom(this.api.recording(on, clear));
      this.store.doc.update((d) => (d ? { ...d, rec: r } : d));
    } catch {
      alert('Could not start recording — check that DSD_NET_LOG_DIR (or DSD_IQ_LOG_DIR) is writable.');
    }
  }
  async toggleAudio(): Promise<void> {
    try {
      const a = await firstValueFrom(this.api.audio(!this.audio().on));
      this.store.doc.update((d) => (d ? { ...d, audio: a } : d));
    } catch {
      alert('Could not start recording audio — check that DSD_NET_AUDIO_DIR (or DSD_NET_LOG_DIR) is writable.');
    }
  }
  readonly audioTitle = computed(() => {
    const a = this.audio();
    return a.on
      ? "Recording each call's voice into " + a.dir + ' (' + mb(a.bytes || 0) + ' of ' + mb(a.cap_bytes || 0) + ', ' + (a.files || 0) + ' files). Click to stop.'
      : "Record each call's decoded voice, to play back here (off by default; encrypted calls are never recorded)";
  });
  async clearAll(): Promise<void> {
    if (!confirm('Forget all calls, talkgroups, radios and networks learned so far' + (this.imports().length ? ', and the imports' : '') + '?')) return;
    await firstValueFrom(this.api.clear()).catch(() => undefined);
    this.store.sel.set(null);
    this.store.setNet(ALL);
  }

  // ---- import (exports added to the live view) ----
  async importFiles(files: File[]): Promise<void> {
    const results: ImportResult[] = [];
    this.importNote.set({ busy: true, results });
    for (const f of files) {
      try {
        results.push(await firstValueFrom(this.api.importExport(f.name, f)));
      } catch (e) {
        const err = e as { error?: ImportResult; message?: string };
        results.push(err.error && err.error.status ? err.error : { name: f.name, status: 'failed', message: String(err.message || e) });
      }
    }
    this.importNote.set({ busy: false, results: [...results] });
  }
  removeImport(id: number): void {
    void firstValueFrom(this.api.removeImport(id)).catch(() => undefined);
  }
  clearImports(): void {
    void firstValueFrom(this.api.clearImports()).catch(() => undefined);
  }
  importSources(x: ImportInfo): string {
    return x.networks + ' networks · ' + x.talkgroups + ' talkgroups · ' + x.radios + ' radios · ' + x.calls + ' calls\n' +
      (x.sources || []).map((s) => sourceText(s, dt)).join('\n');
  }
  importNames(x: ImportInfo): string {
    return (x.sources || []).map((s) => s.name || '?').join(', ');
  }
  statusClass(s: string): string {
    return /^(merged|imported|replaced)$/.test(s) ? 'ok' : s === 'skipped' ? 'skip' : 'bad';
  }

  // ---- open (view saved exports, read-only) ----
  async openFiles(files: File[]): Promise<void> {
    if (!files.length) return;
    if (files.length === 1) {
      const f = files[0];
      let text: string;
      try { text = await readFile(f); } catch { alert('Could not read "' + f.name + '".'); return; }
      this.openText(text, f.name);
      return;
    }
    try {
      const list = await Promise.all(files.map(async (f) => ({ name: f.name, text: await readFile(f) })));
      const res = await firstValueFrom(this.api.merge(list));
      if (res.error) { alert('Merge failed: ' + res.error); return; }
      const merged = res.report.filter((r) => /^(merged|replaced)$/.test(r.status)).length;
      this.openData(res.export, files.length + ' files', res.report);
      if (!merged) alert('None of the files could be merged:\n\n' + res.report.map((r) => r.name + ': ' + r.message).join('\n'));
    } catch (e) {
      const err = e as { error?: { error?: string } };
      alert(err.error?.error ? 'Merge failed: ' + err.error.error : 'Could not merge the files (' + e + ').');
    }
  }
  openText(text: string, name: string): void {
    if (isRecording(text)) {
      alert('"' + name + '" is a recording (the raw decoder input), not an export.\n\n' +
        'Turn it into an export with:\n  net-replay ' + name + ' --export out.json\nthen open out.json here.');
      return;
    }
    let d: unknown;
    try { d = JSON.parse(text); } catch { alert('"' + name + '" is not valid JSON.'); return; }
    this.openData(d as NetExport, name, null);
  }
  /** Shows an export (or a merge of several) read-only. */
  openData(d: NetExport, name: string, report: MergeReportItem[] | null): void {
    const problem = exportProblem(d, name);
    if (problem) { alert(problem); return; }
    if (d.format === 'dsd-net-export' && (d.format_version || 1) > 1)
      alert('This export uses a newer format (v' + d.format_version + '); some details may not show.');
    const exported = d.exported || d.now || 0;
    this.player.close();
    this.store.file.set({ name, exported, source: d.name || d.source || '' });
    this.store.fam.set(null);
    this.store.net.set(ALL);
    this.store.sel.set(null);
    this.store.q.set('');
    this.store.graphCache.fam = null;
    const sources = d.sources || [];
    this.fileReport.set({
      report,
      sources: sources.length > 1 || report ? sources.map((s) => sourceText(s, dt)) : [],
      savedUrl: report ? URL.createObjectURL(new Blob([JSON.stringify(d)], { type: 'application/json' })) : null,
    });
    const doc: NetDoc = { version: -Date.now(), now: d.now || exported, families: d.families };
    this.store.setDoc(doc);
  }
  backToLive(): void {
    this.store.file.set(null);
    this.store.doc.set(null);
    this.store.net.set(ALL);
    this.store.sel.set(null);
    this.store.graphCache.fam = null;
    this.store.fam.set(store.get('fam'));
    this.fileReport.set(null);
    this.status.set('connecting…');
    clearTimeout(this.timer);
    void this.poll();
  }
  protected readonly dt = dt;
  protected readonly Array = Array;
}
