import { Component, computed, effect, inject, signal, viewChild, ChangeDetectionStrategy } from '@angular/core';
import { firstValueFrom } from 'rxjs';
import { dt, hms, mb, mhz, stamp } from '../../core/format';
import { NetCall } from '../../core/models';
import { NetApi } from '../../core/net-api.service';
import { callFile, callKey, heldCalls, isLive } from '../logic/calls';
import { ALL, filterCalls } from '../logic/filters';
import { csv, zipBytes, ZipEntry } from '../logic/zip';
import { AsrService, LANGUAGES } from '../services/asr';
import { Toast } from '../services/toast';
import { NetStore } from '../state/net-store';
import { CardTpl, Col, DataTable, SubTpl } from './data-table';
import { CallAudio, CallDuration, CallTo, CallType, NetChip, RadioLink, Transcript } from './widgets';

/** The audio zip stops at this size (under a typical 30 MB upload limit). */
export const ZIP_MAX = 25 * 1048576;

/** Saves a Blob as a file. */
export function saveBlob(blob: Blob, name: string): void {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}

/**
 * The Calls view: one row per call, newest first, with the toolbar (Pause
 * list, With audio only, speech-to-text settings, audio zip) and each call's
 * transcript on a line under it.
 */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-calls-view',
  imports: [DataTable, Col, CardTpl, SubTpl, CallAudio, CallDuration, CallTo, CallType, NetChip, RadioLink, Transcript],
  templateUrl: './calls-view.html',
})
export class CallsView {
  protected readonly store = inject(NetStore);
  protected readonly asr = inject(AsrService);
  private readonly api = inject(NetApi);
  private readonly toast = inject(Toast);
  private readonly table = viewChild(DataTable<NetCall>);

  protected readonly LANGUAGES = LANGUAGES;
  protected readonly hms = hms;
  protected readonly mhz = mhz;
  readonly zipping = signal<string | null>(null);

  /** The calls listed: the held ones while the list is paused, else the filtered live ones. */
  private readonly held = computed(() => {
    const h = this.store.hold(), ix = this.store.ix();
    if (!h || !ix || h.fam !== this.store.activeFam() || this.store.fileMode()) return null;
    const f = this.store.filter();
    return heldCalls(h.list, h.keys, ix.calls, (cs) => filterCalls(ix, f, cs));
  });
  readonly rows = computed(() => {
    const h = this.held(), ix = this.store.ix();
    return h && ix ? filterCalls(ix, this.store.filter(), h.list) : this.store.calls();
  });
  readonly fresh = computed(() => this.held()?.fresh ?? 0);
  /** Audio controls show once audio is on or some call has audio. */
  readonly hasAudio = computed(() =>
    !!this.store.doc()?.audio?.on || this.store.audioOnly() || !!this.store.ix()?.calls.some((c) => !!c.audio));
  readonly englishOnly = computed(() => /\.en$/.test(this.asr.currentModel()));
  readonly emptyText = computed(() => (this.store.audioOnly() && !this.store.fileMode() ? 'No calls with audio' : 'No calls heard yet') +
    (this.store.activeNet() !== ALL || this.store.q() ? ' for this filter.' : '.') + (this.held() ? ' (The list is paused.)' : ''));

  readonly key = callKey;
  readonly byStart = (c: NetCall) => c.start;
  readonly byDuration = (c: NetCall) => c.last - c.start;
  readonly byFreq = (c: NetCall) => c.freq || 0;
  readonly bySlot = (c: NetCall) => c.slot;
  readonly bySrc = (c: NetCall) => c.src;
  readonly byTgt = (c: NetCall) => c.tgt;
  readonly byAudio = (c: NetCall) => (c.audio && !this.store.fileMode() ? 1 : 0);
  readonly hasFreq = (c: NetCall) => !!c.freq;
  readonly hasSlot = (c: NetCall) => !!c.slot;
  readonly withAudio = (c: NetCall) => !!c.audio && !this.store.fileMode();
  readonly withText = (c: NetCall) => !!c.text;
  readonly withTranscript = (c: NetCall) => !!this.asr.transcript(c.audio)?.t;
  /** The network name already shows the frequency (cards leave the MHz out). */
  readonly netShowsFreq = (c: NetCall) => {
    const n = this.store.ix()?.netByKey[c.net];
    return !!n && !!c.freq && n.label.includes(mhz(c.freq));
  };

  constructor() {
    // A held list belongs to one protocol, and to the live view.
    effect(() => {
      const h = this.store.hold();
      if (h && (h.fam !== this.store.activeFam() || this.store.fileMode())) this.store.setHold(false);
    });
    void this.asr.loadConfig().catch(() => undefined);
  }

  toggleHold(): void {
    this.store.setHold(!this.store.hold());
  }
  setAsrOn(on: boolean): void {
    this.asr.setOn(on);
  }

  /** ⤓ Audio (.zip): the listed calls' audio, in the order shown, up to ZIP_MAX, with calls.csv. */
  async zip(): Promise<void> {
    if (this.zipping()) return;
    const now = this.store.now(), file = this.store.fileMode();
    const rows = (this.table()?.sorted() ?? this.rows()).filter((c) => c.audio && !isLive(c, now, file));
    if (!rows.length) { this.toast.show('No finished calls with audio in the list.'); return; }
    const files: ZipEntry[] = [], names = new Set<string>();
    const table: unknown[][] = [['file', 'start_utc', 'duration_s', 'audio_s', 'network', 'mhz', 'slot', 'from', 'alias', 'to',
      'private', 'emergency', 'encrypted', 'transcript', 'transcript_model']];
    let total = 0, missing = 0, full = false, i = 0;
    for (const c of rows) {
      this.zipping.set('Zipping ' + ++i + ' / ' + rows.length + '…');
      let buf: ArrayBuffer;
      try {
        buf = await firstValueFrom(this.api.audioBytes(c.audio!));
      } catch {
        ++missing;
        continue;
      }
      if (total + buf.byteLength > ZIP_MAX) { full = true; break; }
      let name = callFile(c), k = 1;
      while (names.has(name)) name = callFile(c).replace(/\.wav$/, '_' + ++k + '.wav');
      names.add(name);
      total += buf.byteLength;
      files.push({ name, data: new Uint8Array(buf) });
      const t = this.asr.transcript(c.audio);
      const n = this.store.ix()?.netByKey[c.net];
      table.push([name, dt(c.start), ((c.last - c.start) / 1000).toFixed(1), ((c.audio_ms || 0) / 1000).toFixed(1),
        n ? n.label : c.net, c.freq ? mhz(c.freq) : '', c.slot || '', c.src || '', c.alias || '', c.tgt || '',
        c.priv ? 'yes' : '', c.emerg ? 'yes' : '', c.enc ? 'yes' : '', t ? t.t : '', t ? t.m : '']);
    }
    this.zipping.set(null);
    if (!files.length) { this.toast.show('None of the listed calls’ audio is still on the server.'); return; }
    const n = files.length;
    files.push({ name: 'calls.csv', data: new TextEncoder().encode(csv(table)) });
    saveBlob(new Blob([zipBytes(files) as BlobPart], { type: 'application/zip' }), 'dsd_calls_' + stamp(now) + '.zip');
    this.toast.show('Saved ' + n + ' call' + (n > 1 ? 's' : '') + ' (' + mb(total) + ')' +
      (full ? ' — the first ' + n + ' of ' + rows.length + ' listed; the zip stops at ' + mb(ZIP_MAX) : '') +
      (missing ? '; ' + missing + ' no longer on the server' : '') + '.');
  }
}
