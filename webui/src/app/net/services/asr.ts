import { Injectable, InjectionToken, inject, signal } from '@angular/core';
import { firstValueFrom } from 'rxjs';
import { AsrConfig, NetCall } from '../../core/models';
import { NetApi } from '../../core/net-api.service';
import { store } from '../../core/storage';
import { cleanTranscript } from '../logic/transcript';

// Speech-to-text, on demand: a call is transcribed when it is played. Whisper
// runs in this browser in a Web Worker (/net/asr_worker.js, served by
// dsd-server); the library and model come from the server's /net/asr/ folder
// or -- only if the user chooses -- from the internet. Transcripts are kept in
// this browser (localStorage).

export const CDN_LIB = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@4.3.0/dist/transformers.min.js';
export const LANGUAGES: [string, string][] = [
  ['english', 'English'], ['auto', 'Detect language'], ['spanish', 'Spanish'], ['french', 'French'], ['german', 'German'],
  ['italian', 'Italian'], ['portuguese', 'Portuguese'], ['dutch', 'Dutch'], ['polish', 'Polish'], ['russian', 'Russian'],
  ['ukrainian', 'Ukrainian'], ['arabic', 'Arabic'], ['chinese', 'Chinese'], ['japanese', 'Japanese'], ['korean', 'Korean'],
  ['vietnamese', 'Vietnamese'], ['turkish', 'Turkish'], ['hindi', 'Hindi'],
];

/** The worker the transcription runs in. */
export const ASR_WORKER = new InjectionToken<() => Worker>('ASR_WORKER', {
  factory: () => () => new Worker('/net/asr_worker.js', { type: 'module' }),
});
/** Decodes a WAV and resamples it to Whisper's 16 kHz mono. */
export const DECODE_16K = new InjectionToken<(buf: ArrayBuffer) => Promise<Float32Array>>('DECODE_16K', {
  factory: () => (buf) => new OfflineAudioContext(1, 16000, 16000).decodeAudioData(buf).then((ab) => ab.getChannelData(0).slice()),
});

export interface Transcript {
  t: string;          // the text ('' = no clear speech)
  m: string;          // model
  l: string;          // language
  ms?: number;        // how long it took
  p?: boolean;        // partial: the call was still in progress
}
export type AsrState = 'off' | 'loading' | 'ready' | 'error' | 'nosrc';
interface Job { id: number; name: string; call: NetCall; live: boolean; model?: string; lang?: string; phase?: 'decode' | 'run'; err?: string; }

/** What the now-playing bar says about a call's transcript. */
export interface AsrLine { text: string; muted: boolean; meta?: string; offerInternet?: boolean; }

@Injectable()
export class AsrService {
  private readonly api = inject(NetApi);
  private readonly makeWorker = inject(ASR_WORKER);
  private readonly decode = inject(DECODE_16K);

  readonly cfg = signal<AsrConfig | null>(null);
  readonly on = signal(store.get('asr') !== '0');
  readonly lang = signal<string | null>(store.get('asr.lang'));
  readonly model = signal<string | null>(store.get('asr.model'));
  readonly net = signal(store.get('asr.net') === '1');
  readonly state = signal<AsrState>('off');
  readonly loaded = signal(0);
  readonly err = signal('');
  readonly job = signal<Job | null>(null);
  readonly next = signal<Job | null>(null);
  readonly lastErr = signal<Job | null>(null);
  readonly tx = signal<Record<string, Transcript>>(AsrService.loadTx());

  private worker: Worker | null = null;
  private workerModel: string | null = null;
  private seq = 0;
  private cfgP: Promise<AsrConfig> | null = null;

  private static loadTx(): Record<string, Transcript> {
    try { return JSON.parse(store.get('asr.tx') || '{}') || {}; } catch { return {}; }
  }

  loadConfig(): Promise<AsrConfig> {
    if (!this.cfgP)
      this.cfgP = firstValueFrom(this.api.asrConfig()).then((c) => { this.cfg.set(c); return c; },
        (e) => { this.cfgP = null; throw e; });
    return this.cfgP;
  }
  /** The model in use: the chosen one if the server has it, else the server's default. */
  currentModel(): string {
    const c = this.cfg(), m = this.model();
    return c ? (m && (!c.local || c.models.includes(m)) ? m : c.model) : (m || 'Xenova/whisper-small');
  }
  currentLang(): string {
    return this.lang() || this.cfg()?.language || 'english';
  }
  /** Models offered in the toolbar. */
  models(): string[] {
    const c = this.cfg();
    return !c ? [] : c.local ? c.models : [c.model];
  }
  transcript(audio: string | undefined): Transcript | undefined {
    return audio ? this.tx()[audio] : undefined;
  }

  setOn(on: boolean): void { this.on.set(on); store.set('asr', on ? '1' : '0'); }
  setLang(l: string): void { this.lang.set(l); store.set('asr.lang', l); }
  setModel(m: string): void { this.model.set(m); store.set('asr.model', m); }
  useInternet(): void {
    this.net.set(true);
    store.set('asr.net', '1');
    this.state.set('off');
    this.pump();
  }

  /** Transcribe call `c` (the newest request wins; one runs at a time). */
  transcribe(c: NetCall, live: boolean): void {
    if (!c.audio) return;
    this.next.set({ id: ++this.seq, name: c.audio, call: c, live });
    this.loadConfig().then(() => {
      const r = this.tx()[c.audio!], n = this.next();
      if (n && n.name === c.audio && r && !r.p && r.m === this.currentModel() && r.l === this.currentLang()) this.next.set(null);
      this.pump();
    }, () => {
      this.next.set(null);
      this.state.set('error');
      this.err.set('the server did not answer');
    });
  }

  private source(): { lib: string; wasm: string | null; local: string | null } | null {
    if (this.cfg()?.local) return { lib: '/net/asr/transformers.min.js', wasm: '/net/asr/ort/', local: '/net/asr/models/' };
    if (this.net()) return { lib: CDN_LIB, wasm: null, local: null };
    return null;
  }

  private fail(msg: string): void {
    this.state.set('error');
    this.err.set(msg);
    this.worker?.terminate();
    this.worker = null;
    this.workerModel = null;
    this.job.set(null);
    this.next.set(null);
  }

  private startWorker(model: string): void {
    this.worker?.terminate();
    const src = this.source()!;
    const w = this.makeWorker();
    this.worker = w;
    this.workerModel = model;
    this.state.set('loading');
    this.loaded.set(0);
    this.err.set('');
    w.onmessage = (e: MessageEvent) => this.onMessage(e.data);
    w.onerror = (e: ErrorEvent) => this.fail('the speech-to-text worker stopped' + (e?.message ? ' (' + e.message + ')' : ''));
    const abs = (u: string) => new URL(u, location.href).href;
    w.postMessage({ cmd: 'load', lib: abs(src.lib), wasm: src.wasm ? abs(src.wasm) : null, local: src.local, model });
  }

  private onMessage(m: { type: string; id?: number; text?: string; ms?: number; msg?: string; cmd?: string; loaded?: number }): void {
    const j = this.job();
    if (m.type === 'progress') { this.loaded.set(m.loaded || 0); return; }
    if (m.type === 'ready') { this.state.set('ready'); this.pump(); return; }
    if (m.type === 'error' && m.cmd === 'load') { this.fail(m.msg || 'failed'); return; }
    if (!j || j.id !== m.id) return;
    this.job.set(null);
    if (m.type === 'result') {
      this.tx.update((tx) => ({ ...tx, [j.name]: { t: cleanTranscript(m.text), m: j.model!, l: j.lang!, ms: m.ms, ...(j.live ? { p: true } : {}) } }));
      this.saveTx();
    } else if (m.type === 'error') {
      this.lastErr.set({ ...j, err: m.msg });
    }
    this.pump();
  }

  /** Keeps the last ~1000 finished transcripts. */
  private saveTx(): void {
    const tx = { ...this.tx() };
    const k = Object.keys(tx);
    if (k.length > 1000) k.slice(0, k.length - 800).forEach((x) => delete tx[x]);
    const keep: Record<string, Transcript> = {};
    for (const x of Object.keys(tx)) if (!tx[x].p) keep[x] = tx[x];
    store.set('asr.tx', JSON.stringify(keep));
  }

  /** Starts the next job when the worker is free. */
  pump(): void {
    const next = this.next();
    if (this.job() || !next || !this.cfg()) return;
    if (!this.source()) { this.state.set('nosrc'); return; }
    const model = this.currentModel();
    if (!this.worker || this.workerModel !== model) this.startWorker(model);
    if (this.state() !== 'ready') return;
    const j: Job = { ...next, model, lang: this.currentLang(), phase: 'decode' };
    this.job.set(j);
    this.next.set(null);
    firstValueFrom(this.api.audioBytes(j.name))
      .then((buf) => this.decode(buf))
      .then((pcm) => {
        if (this.job()?.id !== j.id || !this.worker) return;
        this.job.set({ ...j, phase: 'run' });
        const language = /\.en$/.test(model) || j.lang === 'auto' ? null : j.lang;
        this.worker.postMessage({ cmd: 'run', id: j.id, pcm, language }, [pcm.buffer]);
      })
      .catch((e) => {
        if (this.job()?.id !== j.id) return;
        this.job.set(null);
        this.lastErr.set({ ...j, err: e?.status === 404 ? 'its audio is no longer on the server' : String(e?.message || e) });
        this.pump();
      });
  }

  /** The now-playing bar's transcript line for the call with audio `name`. */
  describe(name: string, mb: (n: number) => string): AsrLine {
    const r = this.tx()[name];
    const job = this.job(), next = this.next();
    const j = job?.name === name ? job : next?.name === name ? next : null;
    if (r && !j)
      return { text: r.t || '(no clear speech recognized)', muted: !r.t,
               meta: r.m.replace(/^.*\//, '') + (r.ms ? ' · ' + (r.ms / 1000).toFixed(1) + ' s' : '') + (r.p ? ' · partial (call still in progress)' : '') };
    if (!this.on()) return { text: 'Transcribe on play is off (Calls toolbar).', muted: true };
    if (this.state() === 'nosrc')
      return { text: 'Speech-to-text needs its files on the server (tools/get_asr_assets.sh) — or', muted: true, offerInternet: true };
    if (this.state() === 'error') return { text: 'Speech-to-text failed: ' + this.err(), muted: true };
    const le = this.lastErr();
    if (le && le.name === name && !j) return { text: 'Could not transcribe this call: ' + le.err, muted: true };
    if (!j) return { text: '', muted: true };
    if (this.state() === 'loading')
      return { text: 'Loading the speech model' + (this.loaded() ? ' — ' + mb(this.loaded()) : '') + ' (once per visit)…', muted: true };
    return { text: j === job && j.phase === 'run' ? 'Transcribing…' : 'Waiting to transcribe…', muted: true };
  }
}
