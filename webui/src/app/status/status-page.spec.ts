import { provideHttpClient, withFetch } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { StatusDoc } from '../core/models';
import { StatusPage } from './status-page';

export function statusDoc(over: Partial<StatusDoc> = {}): StatusDoc {
  return {
    total_sessions: 7, current_sessions: 2, active_pipelines: 1, uptime_seconds: 3725, log_lines: 42,
    iq_logging: false, started: '2026-10-02 07:00:00Z', by_protocol: { p25p1: 1, dmr: 2 },
    sessions: [
      { id: 3, remote: '10.0.0.5:5000', protocol: 'pager-auto', chain: 'multimon', protocols_used: ['dmr', 'pager-auto'],
        protocols_requested: ['dmr', 'nxdn48', 'pager-auto'], active: true, connected: '2026-10-02 07:01:00Z', duration_seconds: 65 },
      { id: 4, remote: '', protocol: 'p25p1', chain: 'dsd-fme', protocols_used: [], protocols_requested: [], active: false,
        connected: '2026-10-02 07:02:00Z', duration_seconds: 5 },
    ],
    history: [{ id: 1, remote: '10.0.0.9:1', protocol: 'dmr', chain: 'dsd-fme', connected: '2026-10-02 06:00:00Z',
                ended: '2026-10-02 06:30:00Z', protocols_used: ['dmr'], protocols_requested: ['dmr'], duration_seconds: 1800 }],
    protocols: [{ protocol: 'dmr', chain: 'dsd-fme', requests: 3, starts: 2, failed: 1, sessions: 2, active: 1,
                  decode_seconds: 90, first_requested: '2026-10-02 06:00:00Z', last_requested: '' }],
    ...over,
  };
}

describe('StatusPage', () => {
  let fixture: ComponentFixture<StatusPage>;
  let http: HttpTestingController;
  let el: HTMLElement;
  const q = (sel: string) => el.querySelector(sel) as HTMLElement;
  const text = (sel: string) => q(sel).textContent!.replace(/\s+/g, ' ').trim();

  /** Lets pending promises resolve, then renders. */
  async function settle() {
    await new Promise((r) => setTimeout(r, 0));
    await fixture.whenStable();
  }
  async function flushStatus(doc: StatusDoc = statusDoc()) {
    http.expectOne('/status.json').flush(doc);
    await fixture.whenStable();
  }

  beforeEach(async () => {
    TestBed.configureTestingModule({
      providers: [provideHttpClient(withFetch()), provideHttpClientTesting(), provideRouter([])],
    });
    http = TestBed.inject(HttpTestingController);
    fixture = TestBed.createComponent(StatusPage);
    fixture.componentInstance.pollMs = 1e9;      // the tests drive tick() themselves
    el = fixture.nativeElement;
    await Promise.resolve();                     // the first poll starts
  });
  afterEach(() => fixture.destroy());
  /** One more poll, answered with `body` (or an error). */
  async function poll(body: unknown, status = 200) {
    const done = fixture.componentInstance.tick();
    const req = http.expectOne('/status.json');
    if (status === 200) req.flush(body as object); else req.flush('down', { status, statusText: 'Bad Gateway' });
    await done;
    await fixture.whenStable();
  }

  it('renders the counts, uptime and live sessions', async () => {
    await flushStatus();
    expect(text('[data-test=cur]')).toBe('2');
    expect(text('[data-test=tot]')).toBe('7');
    expect(text('[data-test=act]')).toBe('1');
    expect(text('[data-test=uptime]')).toBe('1h 2m 5s');
    expect(text('[data-test=live]')).toBe('live');
    const rows = el.querySelectorAll('[data-test=sessions] tbody tr');
    expect(rows.length).toBe(2);
    expect(rows[0].textContent).toContain('dmr → nxdn48 ✗ → pager-auto');
    expect(rows[0].textContent).toContain('decoding');
    expect(rows[1].textContent).toContain('-');          // no remote
    expect(rows[1].textContent).toContain('idle');
  });

  it('lists active decodes by protocol, sorted', async () => {
    await flushStatus();
    const tags = Array.from(el.querySelectorAll('[data-test=byproto] .tag')).map((t) => t.textContent!.trim());
    expect(tags).toEqual(['dmr 2', 'p25p1 1']);
  });

  it('hides the by-protocol strip and shows empty rows when idle', async () => {
    await flushStatus(statusDoc({ by_protocol: {}, sessions: [], history: [], protocols: [] }));
    expect(q('[data-test=byproto]')).toBeNull();
    expect(text('[data-test=sessions] tbody')).toBe('no clients connected');
  });

  it('switches to the protocols and history tabs', async () => {
    await flushStatus();
    (el.querySelectorAll('.tab')[1] as HTMLElement).click();
    await fixture.whenStable();
    const p = text('[data-test=protocols] tbody');
    expect(p).toContain('1 decoding');
    expect(p).toContain('1m 30s');
    expect(p).toMatch(/-$/);                             // never last-requested
    (el.querySelectorAll('.tab')[2] as HTMLElement).click();
    await fixture.whenStable();
    expect(text('[data-test=history] tbody')).toContain('30m 0s');
  });

  it('loads the log on its tab; pause freezes it; clear empties it', async () => {
    await flushStatus();
    (el.querySelectorAll('.tab')[3] as HTMLElement).click();
    http.expectOne('/log.json').flush({ log: [{ session: 3, time: '07:01:02Z', text: '{"type":"event"}' }] });
    await fixture.whenStable();
    expect(text('[data-test=log] tbody')).toContain('{"type":"event"}');
    expect(text('[data-test=log-count]')).toBe('1');

    q('[data-test=log-pause]').click();
    await fixture.whenStable();
    expect(text('[data-test=log-pause]')).toBe('Resume');
    await poll(statusDoc());                             // next poll: status only, no log fetch
    http.expectNone('/log.json');

    q('[data-test=log-clear]').click();
    http.expectOne('/log/clear').flush({ log: [] });
    await fixture.whenStable();
    expect(text('[data-test=log] tbody')).toBe('no frames logged yet');
  });

  it('flips the IQ capture switch through the server', async () => {
    await flushStatus();
    const box = q('[data-test=iq]') as HTMLInputElement;
    expect(box.checked).toBe(false);
    box.checked = true;
    box.dispatchEvent(new Event('change'));
    await fixture.whenStable();
    expect(text('[data-test=iq-note]')).toBe('enabling capture…');
    http.expectOne('/iq_log/on').flush({ iq_log_enabled: true });
    await settle();
    expect(box.checked).toBe(true);
    expect(text('[data-test=iq-note]')).toBe('capturing IQ for active & new sessions');
  });

  it('keeps polling and reports a lost connection', async () => {
    await flushStatus();
    await poll(null, 502);
    expect(text('[data-test=live]')).toBe('disconnected — retrying');
    expect(text('[data-test=cur]')).toBe('2');          // the last data stays up
    await poll(statusDoc({ current_sessions: 5 }));
    expect(text('[data-test=live]')).toBe('live');
    expect(text('[data-test=cur]')).toBe('5');
  });
});
