import { HttpClient } from '@angular/common/http';
import { Injectable, inject } from '@angular/core';
import { Observable } from 'rxjs';
import { LogDoc, StatusDoc } from './models';

/** The status page's endpoints (/status.json, /log.json, /log/clear, /iq_log/on|off). */
@Injectable({ providedIn: 'root' })
export class StatusApi {
  private readonly http = inject(HttpClient);
  private readonly fresh = { cache: 'no-store' } as const;

  status(): Observable<StatusDoc> {
    return this.http.get<StatusDoc>('/status.json', this.fresh);
  }
  log(): Observable<LogDoc> {
    return this.http.get<LogDoc>('/log.json', this.fresh);
  }
  /** Empties the server's log ring; answers with the (now empty) log. */
  clearLog(): Observable<LogDoc> {
    return this.http.get<LogDoc>('/log/clear', this.fresh);
  }
  /** Flips the global IQ-capture switch; answers with the state now in effect. */
  setIqLogging(on: boolean): Observable<{ iq_log_enabled: boolean }> {
    return this.http.get<{ iq_log_enabled: boolean }>('/iq_log/' + (on ? 'on' : 'off'), this.fresh);
  }
}
