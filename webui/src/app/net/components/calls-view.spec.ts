import { ComponentFixture, TestBed } from '@angular/core/testing';
import { NetDoc } from '../../core/models';
import { call, family, netDoc } from '../logic/fixture.spec-helper';
import { NetPage } from '../net-page';
import { Breakpoints } from '../services/breakpoints';
import { FakeBreakpoints, explorerProviders, http, tick } from '../testing.spec-helper';

describe('Calls view, table and now-playing bar', () => {
  let fixture: ComponentFixture<NetPage>;
  let el: HTMLElement;
  const $ = (s: string) => el.querySelector(s) as HTMLElement;
  const $$ = (s: string) => Array.from(el.querySelectorAll(s)) as HTMLElement[];
  const rows = () => $$('app-calls-view tbody tr:not(.sub)');
  const asrCfg = { local: true, lib: true, models: ['Xenova/whisper-small'], model: 'Xenova/whisper-small', language: 'english' };

  async function render() {
    await tick();
    await fixture.whenStable();
  }
  async function serve(d: NetDoc = netDoc()) {
    http().expectOne('/net.json').flush(d);
    await render();
    http().match('/net/asr/config.json').forEach((r) => r.flush(asrCfg));
    await render();
  }
  async function poll(d: NetDoc) {
    const p = fixture.componentInstance.poll();
    http().expectOne('/net.json').flush(d);
    await p;
    await render();
  }

  beforeEach(async () => {
    TestBed.configureTestingModule({ providers: explorerProviders() });
    fixture = TestBed.createComponent(NetPage);
    fixture.componentInstance.pollMs = 1e9;
    el = fixture.nativeElement;
    await tick();
  });
  afterEach(() => fixture.destroy());

  it('Pause list holds the rows and counts new calls; Resume shows them', async () => {
    await serve();
    $('[data-test=hold]').click();
    await render();
    expect($('[data-test=hold]').textContent!.trim()).toBe('▶ Resume list');
    const d = netDoc(), F = d.families['dmr'];
    F.calls = [call({ id: 9, src: '200', tgt: '10', start: 1_295_000, last: 1_296_000 }), ...F.calls];
    await poll(d);
    expect(rows().length).toBe(5);
    expect($('[data-test=held]').textContent!.trim()).toBe('Paused · 1 new call since');
    $('[data-test=hold]').click();
    await render();
    expect(rows().length).toBe(6);
    expect($('[data-test=held]')).toBeNull();
  });

  it('With audio only, and sorting by the Audio column', async () => {
    await serve();
    const th = () => $$('app-calls-view th').find((h) => h.textContent!.startsWith('Audio'))!;
    th().click();
    await render();
    expect(rows()[0].querySelector('.play')).not.toBeNull();     // calls with audio first
    ($('[data-test=audonly]') as HTMLInputElement).click();
    await render();
    expect(rows().length).toBe(1);
    expect(localStorage.getItem('netx.audonly')).toBe('1');
  });

  it('shows the download link with a descriptive file name', async () => {
    await serve();
    const a = $('app-calls-view a.dl') as HTMLAnchorElement;
    expect(a.getAttribute('href')).toBe('/net/audio/call_1290000_ab_5.wav');
    expect(a.getAttribute('download')).toBe('call_19700101T002130Z_460.0250MHz_s1_to_400_from_300.wav');
  });

  it('plays a call into the now-playing bar, transcribes it, and shows the transcript under the call', async () => {
    await serve();
    ($('app-calls-view .play') as HTMLElement).click();
    await render();
    const bar = () => ($('[data-test=np]')?.textContent || '').replace(/\s+/g, ' ');
    expect(bar()).toMatch(/300 UNIT 21B → ⇄ 400/);       // with the radio's alias
    expect(bar()).toContain('460.0250 MHz');
    expect(document.body.classList.contains('np-on')).toBe(true);
    // the transcription runs (worker stand-in)
    const { FakeWorker } = await import('../testing.spec-helper');
    FakeWorker.last!.reply({ type: 'ready' });
    http().expectOne('/net/audio/call_1290000_ab_5.wav').flush(new ArrayBuffer(16));
    await render();
    FakeWorker.last!.reply({ type: 'result', id: FakeWorker.last!.sent[1]['id'], text: 'Unit three hundred, copy.', ms: 4100 });
    await render();
    expect($('[data-test=np-tx]').textContent).toContain('Unit three hundred, copy.');
    expect($('app-calls-view tr.sub .stt').textContent).toBe('Unit three hundred, copy.');
    ($('.np .x') as HTMLElement).click();
    await render();
    expect($('[data-test=np]')).toBeNull();
    expect(document.body.classList.contains('np-on')).toBe(false);
  });

  it('zips the listed calls\' audio with a CSV', async () => {
    await serve();
    const saved: Blob[] = [];
    const created = vi.spyOn(URL, 'createObjectURL').mockImplementation((b) => { saved.push(b as Blob); return 'blob:x'; });
    vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => undefined);
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => undefined);
    $('[data-test=zip]').click();
    await tick();
    http().expectOne('/net/audio/call_1290000_ab_5.wav').flush(new Uint8Array([82, 73, 70, 70]).buffer);
    await render();
    expect(created).toHaveBeenCalled();
    const bytes = new Uint8Array(await saved[0].arrayBuffer());
    const txt = new TextDecoder().decode(bytes);
    expect(txt).toContain('call_19700101T002130Z_460.0250MHz_s1_to_400_from_300.wav');
    expect(txt).toContain('file,start_utc,duration_s');
    expect($('.toast').textContent).toContain('Saved 1 call');
  });

  it('on a phone: call cards and a sort menu', async () => {
    (TestBed.inject(Breakpoints) as unknown as FakeBreakpoints).phone.set(true);
    await serve();
    expect($$('app-calls-view .ccard').length).toBe(5);
    expect($('app-calls-view table')).toBeNull();
    const sel = $('app-calls-view .sortbar select') as HTMLSelectElement;
    expect(sel.options.length).toBeGreaterThan(5);
    sel.value = Array.from(sel.options).find((o) => o.text.startsWith('From') && o.text.endsWith('▲'))!.value;
    sel.dispatchEvent(new Event('change'));
    await render();
    expect($$('app-calls-view .ccard')[0].textContent).toContain('100');
    expect($('app-calls-view .sortbar').textContent).toContain('5 rows');
  });


});
