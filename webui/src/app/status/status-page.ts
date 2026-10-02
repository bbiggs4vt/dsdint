import { Component, DestroyRef, computed, inject, signal } from '@angular/core';
import { RouterLink } from '@angular/router';
import { firstValueFrom } from 'rxjs';
import { humanDuration, protocolTrail } from '../core/format';
import { LogEntry, StatusDoc } from '../core/models';
import { StatusApi } from '../core/status-api.service';

export type StatusTab = 'sessions' | 'protocols' | 'history' | 'log';
export const STATUS_POLL_MS = 1000;
const IQ_NOTE_ON = 'capturing IQ for active & new sessions';

/**
 * The server status page (port of the built-in "/" page): session counts, the
 * IQ-capture switch, active decodes by protocol, and tabs for live sessions,
 * run-wide protocol usage, finished sessions and the outbound-frame log.
 * Polls /status.json every second (and /log.json while the Log tab is open).
 */
@Component({
  selector: 'app-status-page',
  imports: [RouterLink],
  templateUrl: './status-page.html',
  styleUrl: './status-page.css',
})
export class StatusPage {
  private readonly api = inject(StatusApi);

  readonly doc = signal<StatusDoc | null>(null);
  readonly log = signal<LogEntry[]>([]);
  readonly tab = signal<StatusTab>('sessions');
  readonly logPaused = signal(false);
  readonly connected = signal(true);
  readonly iqNote = signal('');
  readonly iqChecked = signal(false);
  /** After a click, the poll doesn't overwrite the switch for a moment. */
  private iqPendingUntil = 0;
  /** Poll interval (ms); tests stretch it to drive tick() themselves. */
  pollMs = STATUS_POLL_MS;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private stopped = false;

  readonly byProtocol = computed(() => {
    const bp = this.doc()?.by_protocol || {};
    return Object.keys(bp).sort().map((k) => ({ protocol: k, n: bp[k] }));
  });
  readonly logCount = computed(() => (this.tab() === 'log' ? this.log().length : (this.doc()?.log_lines ?? 0)));

  protected readonly humanDuration = humanDuration;
  protected readonly trail = protocolTrail;

  constructor() {
    inject(DestroyRef).onDestroy(() => {
      this.stopped = true;
      clearTimeout(this.timer);
    });
    queueMicrotask(() => void this.tick());
  }

  /** One poll: the status, then the log if its tab is open; then schedule the next. */
  async tick(): Promise<void> {
    try {
      this.apply(await firstValueFrom(this.api.status()));
      this.connected.set(true);
    } catch {
      this.connected.set(false);
    }
    if (this.tab() === 'log') await this.fetchLog();
    if (!this.stopped) this.timer = setTimeout(() => void this.tick(), this.pollMs);
  }

  apply(d: StatusDoc): void {
    this.doc.set(d);
    if (Date.now() > this.iqPendingUntil) {
      this.iqChecked.set(!!d.iq_logging);
      this.iqNote.set(d.iq_logging ? IQ_NOTE_ON : '');
    }
  }

  async fetchLog(): Promise<void> {
    if (this.logPaused()) return;          // frozen for inspection
    try {
      this.log.set((await firstValueFrom(this.api.log())).log || []);
    } catch { /* next poll */ }
  }

  showTab(t: StatusTab): void {
    this.tab.set(t);
    if (t === 'log') void this.fetchLog();
  }

  toggleLogPause(): void {
    this.logPaused.update((p) => !p);
    if (!this.logPaused()) void this.fetchLog();   // catch up on resume
  }

  async clearLog(): Promise<void> {
    try {
      this.log.set((await firstValueFrom(this.api.clearLog())).log || []);
    } catch { /* ignore */ }
  }

  async setIq(on: boolean): Promise<void> {
    this.iqPendingUntil = Date.now() + 2500;
    this.iqChecked.set(on);
    this.iqNote.set(on ? 'enabling capture…' : 'stopping capture…');
    try {
      const r = await firstValueFrom(this.api.setIqLogging(on));
      this.iqChecked.set(!!r.iq_log_enabled);
      this.iqNote.set(r.iq_log_enabled ? IQ_NOTE_ON : '');
    } catch {
      this.iqNote.set('request failed');
    }
  }
}
