import { HttpClient } from '@angular/common/http';
import { Injectable, inject } from '@angular/core';
import { Observable } from 'rxjs';
import { AsrConfig, AudioStatus, ImportResult, MergeResponse, NetDoc, RecStatus } from './models';

/** The network explorer's endpoints (see PROTOCOL.md, "/net"). */
@Injectable({ providedIn: 'root' })
export class NetApi {
  private readonly http = inject(HttpClient);
  private readonly fresh = { cache: 'no-store' } as const;

  /** The live model (gzip-compressed on the wire; the browser inflates it). */
  net(): Observable<NetDoc> {
    return this.http.get<NetDoc>('/net.json', this.fresh);
  }
  /** Record button: start (optionally clearing first) or stop recording. */
  recording(on: boolean, clear = false): Observable<RecStatus> {
    return this.http.get<RecStatus>(on ? '/net/log/on' + (clear ? '?clear=1' : '') : '/net/log/off', this.fresh);
  }
  /** Audio switch: start or stop recording each call's voice. */
  audio(on: boolean): Observable<AudioStatus> {
    return this.http.get<AudioStatus>(on ? '/net/audio/on' : '/net/audio/off', this.fresh);
  }
  clear(): Observable<{ ok: boolean }> {
    return this.http.get<{ ok: boolean }>('/net/clear', this.fresh);
  }
  /** Adds an export (JSON or gzip, sent as is) to the live view. */
  importExport(name: string, body: Blob): Observable<ImportResult> {
    return this.http.post<ImportResult>('/net/import?name=' + encodeURIComponent(name), body, this.fresh);
  }
  removeImport(id: number): Observable<{ ok: boolean }> {
    return this.http.get<{ ok: boolean }>('/net/imports/remove?id=' + id, this.fresh);
  }
  clearImports(): Observable<{ ok: boolean }> {
    return this.http.get<{ ok: boolean }>('/net/imports/clear', this.fresh);
  }
  /** Merges several exports without touching the live data. */
  merge(files: { name: string; text: string }[]): Observable<MergeResponse> {
    return this.http.post<MergeResponse>('/net/merge', { files }, this.fresh);
  }
  asrConfig(): Observable<AsrConfig> {
    return this.http.get<AsrConfig>('/net/asr/config.json', this.fresh);
  }
  /** A call's recorded audio file. */
  audioBytes(name: string): Observable<ArrayBuffer> {
    return this.http.get(NetApi.audioUrl(name), { responseType: 'arraybuffer', ...this.fresh });
  }
  static audioUrl(name: string): string {
    return '/net/audio/' + encodeURIComponent(name);
  }
}
