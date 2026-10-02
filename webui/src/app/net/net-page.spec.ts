import { ComponentFixture, TestBed } from '@angular/core/testing';
import { NetDoc } from '../core/models';
import { family, netDoc } from './logic/fixture.spec-helper';
import { NetPage } from './net-page';
import { FakeBreakpoints, explorerProviders, http, tick } from './testing.spec-helper';
import { Breakpoints } from './services/breakpoints';

describe('NetPage (explorer)', () => {
  let fixture: ComponentFixture<NetPage>;
  let page: NetPage;
  let el: HTMLElement;
  const $ = (s: string) => el.querySelector(s) as HTMLElement;
  const $$ = (s: string) => Array.from(el.querySelectorAll(s)) as HTMLElement[];
  const text = (s: string) => ($(s)?.textContent || '').replace(/\s+/g, ' ').trim();

  const cardTexts = () => $$('[data-test=cards] .card').map((c) => c.querySelector('.n')!.textContent!.trim() + ' ' + c.querySelector('.l')!.textContent!.trim());
  async function render() {
    await tick();
    await fixture.whenStable();
  }
  /** Answers the poll in flight (and the speech config the Calls view asks for). */
  async function serve(d: NetDoc = netDoc()) {
    http().match('/net/asr/config.json').forEach((r) => r.flush({ local: false, lib: false, models: [], model: 'Xenova/whisper-small', language: 'english' }));
    http().expectOne('/net.json').flush(d);
    await render();
    http().match('/net/asr/config.json').forEach((r) => r.flush({ local: false, lib: false, models: [], model: 'Xenova/whisper-small', language: 'english' }));
  }
  async function view(label: string) {
    $$('[data-test=viewtabs] .tab').find((t) => t.textContent!.startsWith(label))!.click();
    await render();
  }

  beforeEach(async () => {
    TestBed.configureTestingModule({ providers: explorerProviders() });
    fixture = TestBed.createComponent(NetPage);
    page = fixture.componentInstance;
    page.pollMs = 1e9;                                   // the tests poll by hand
    el = fixture.nativeElement;
    await tick();
  });
  afterEach(() => fixture.destroy());

  it('waits for traffic', async () => {
    await serve(netDoc({ families: {} }));
    expect(text('[data-test=empty]')).toContain('Waiting for digital-voice traffic');
    expect(text('[data-test=live]')).toMatch(/^live · updated \d\d:\d\d:\d\dZ$/);
  });

  it('shows protocols, stat cards, network chips and calls', async () => {
    await serve();
    expect(text('[data-test=famtabs]')).toBe('DMR 4 radios · 1 live');
    const cards = cardTexts();
    expect(cards).toEqual(['2 Networks', '1 Sites', '2 Talkgroups', '4 Radios', '5 Calls (recent)', '0.3 Calls / s', '1 Live calls']);
    expect($$('[data-test=netbar] .chip').map((c) => c.textContent!.trim())).toEqual(['All networks', 'Network 95', 'Color Code 1 · 434.4250 MHz2']);
    const rows = $$('app-calls-view tbody tr:not(.sub)');
    expect(rows.length).toBe(5);
    expect(rows[0].textContent).toContain('⇄ 400');                       // private
    expect(rows[1].textContent).toContain('K. REYES');                    // the call's alias
    expect($('app-calls-view tbody').textContent).toContain('ENCRYPTED');
    expect(text('[data-test=viewtabs]')).toContain('Calls 5');
  });

  it('filters by search and by network', async () => {
    await serve();
    page.search('reyes');
    await tick(250);
    await fixture.whenStable();
    expect($$('app-calls-view tbody tr:not(.sub)').length).toBe(2);
    page.search('');
    await tick(250);
    $$('[data-test=netbar] .chip')[2].click();          // the channel network
    await render();
    expect($$('app-calls-view tbody tr:not(.sub)').length).toBe(1);
    expect(cardTexts()[0]).toBe('1 Networks');
    expect(text('app-details')).toContain('Show all networks');           // its details opened too
    $$('[data-test=netbar] .chip')[0].click();
    await render();
    expect($$('app-calls-view tbody tr:not(.sub)').length).toBe(5);
  });

  it('opens a radio\'s details from a call', async () => {
    await serve();
    $$('app-calls-view app-radio a').find((a) => a.textContent!.startsWith('200'))!.click();
    await render();
    const d = text('app-details');
    expect(d).toContain('Radio 200');
    expect(d).toContain('Talkgroups used');
    expect(d).toContain('Shares talkgroups with');
    expect(d).toMatch(/100.{0,40}1 TG/);
    expect($$('app-details .lst').length).toBe(3);        // talkgroups, shared, recent calls
    expect(d).toMatch(/Private calls withNone\./);
  });

  it('shows the talkgroups, radios, networks and links views', async () => {
    await serve();
    await view('Talkgroups');
    const tgs = $$('app-tgs-view tbody tr');
    expect(tgs.length).toBe(2);
    expect(tgs[0].textContent).toContain('TG 9');                         // last heard first
    tgs[1].click();
    await render();
    expect(text('app-details')).toContain('Talkgroup 10');
    expect($('app-tgs-view tbody tr.sel').textContent).toContain('TG 10');

    await view('Radios');
    expect($$('app-radios-view tbody tr').length).toBe(4);
    expect(text('app-radios-view tbody')).toContain('UNIT 21B');

    await view('Networks');
    const nets = $$('app-nets-view tbody tr');
    expect(nets.map((r) => r.querySelector('.badge')!.textContent)).toEqual(['strong', 'channel']);

    await view('Links');
    const links = text('app-links-view');
    expect(links).toContain('Community 1');
    expect(links).toContain('Seen on more than one network — possible links between networks (2)');
  });

  it('draws the graph', async () => {
    await serve();
    await view('Graph');
    await tick(20);
    expect(text('[data-test=g-note]')).toBe('6 nodes · 5 links');
    expect(el.querySelectorAll('#gsvg .gnode').length).toBe(6);
    expect(el.querySelectorAll('#gsvg .glink.priv').length).toBe(1);
  });

  it('sorts a table by a column, and back', async () => {
    await serve();
    const th = () => $$('app-calls-view th').find((h) => h.textContent!.startsWith('From'))!;
    th().click();
    await render();
    expect(th().textContent).toContain('▼');
    expect($$('app-calls-view tbody tr:not(.sub)')[0].textContent).toContain('300');
    th().click();
    await render();
    expect(th().textContent).toContain('▲');
    expect($$('app-calls-view tbody tr:not(.sub)')[0].textContent).toContain('100');
  });

  it('records, switches audio, pauses and clears through the server', async () => {
    await serve();
    vi.spyOn(window, 'confirm').mockReturnValue(true);
    $('[data-test=rec]').click();
    http().expectOne('/net/log/on?clear=1').flush({ on: true, file: 'net_x.jsonl.gz', file_bytes: 2048 });
    await render();
    expect(text('[data-test=rec]')).toBe('■ Stop recording');
    expect(text('.recst')).toContain('REC net_x.jsonl.gz · 2 KB');

    $('[data-test=aud]').click();
    http().expectOne('/net/audio/on').flush({ on: true, dir: '/a', bytes: 0, cap_bytes: 1 << 30, files: 0 });
    await render();
    expect(text('[data-test=aud]')).toBe('♫ Audio on');

    $('[data-test=pause]').click();
    await render();
    expect(text('[data-test=live]')).toBe('paused — view frozen');
    await page.poll();
    http().expectNone('/net.json');                     // paused: no poll
    $('[data-test=pause]').click();

    $('[data-test=clear]').click();
    await tick();
    http().expectOne('/net/clear').flush({ ok: true });
  });

  it('lists imports and removes them', async () => {
    await serve(netDoc({ imports: [{ id: 3, label: 'north.json', exported: 1, sources: [{ name: 'rx-north' }], networks: 1, talkgroups: 2, radios: 3, calls: 4 }] }));
    expect(text('[data-test=impbar]')).toContain('Including 1 import');
    expect(text('[data-test=impbar]')).toContain('north.json (rx-north)');
    ($('[data-test=impbar] .imp-item a')).click();
    http().expectOne('/net/imports/remove?id=3').flush({ ok: true });
  });

  it('imports files and reports what happened', async () => {
    await serve();
    const done = page.importFiles([new File(['{}'], 'a.json'), new File(['{}'], 'b.json')]);
    await tick();
    http().expectOne('/net/import?name=a.json').flush({ name: 'a.json', status: 'imported', message: 'ok', id: 1 });
    await tick();
    http().expectOne('/net/import?name=b.json').flush({ name: 'b.json', status: 'refused', message: 'overlaps' }, { status: 200, statusText: 'OK' });
    await done;
    await render();
    const note = text('[data-test=impnote]');
    expect(note).toContain('a.json — ok');
    expect(note).toContain('b.json — overlaps');
    expect($$('[data-test=impnote] .st').map((e) => e.className.trim() + ':' + e.textContent)).toEqual(['st ok:imported', 'st bad:refused']);
  });

  it('opens a saved export read-only, and goes back to live', async () => {
    await serve();
    const F = family();
    F.calls = F.calls.slice(0, 2);
    page.openText(JSON.stringify({ format: 'dsd-net-export', format_version: 1, exported: 1_300_000, now: 1_300_000, name: 'rx-south', families: { dmr: F } }), 'south.json');
    await render();
    expect(text('[data-test=filebar]')).toContain('Viewing export south.json');
    expect(text('[data-test=filebar]')).toContain('by rx-south');
    expect(text('[data-test=live]')).toBe('file view');
    expect(document.body.classList.contains('filemode')).toBe(true);
    expect($$('app-calls-view tbody tr:not(.sub)').length).toBe(2);
    $('[data-test=back]').click();
    await tick();
    expect(document.body.classList.contains('filemode')).toBe(false);
    http().expectOne('/net.json').flush(netDoc());
    await render();
    expect($$('app-calls-view tbody tr:not(.sub)').length).toBe(5);
  });

  it('refuses recordings and other files with an explanation', async () => {
    await serve();
    const alert = vi.spyOn(window, 'alert').mockImplementation(() => undefined);
    page.openText('{"op":"header","v":1}', 'net_x.jsonl');
    expect(alert.mock.calls[0][0]).toContain('is a recording');
    page.openText('not json', 'x.txt');
    expect(alert.mock.calls[1][0]).toContain('is not valid JSON');
    page.openText('{"a":1}', 'y.json');
    expect(alert.mock.calls[2][0]).toContain('not a dsd-server explorer export');
  });

  it('merges several files through the server', async () => {
    await serve();
    const done = page.openFiles([new File(['{"a":1}'], 'a.json'), new File(['{"b":2}'], 'b.json')]);
    await tick(10);
    const req = http().expectOne('/net/merge');
    expect(req.request.body.files.map((f: { name: string }) => f.name)).toEqual(['a.json', 'b.json']);
    req.flush({ report: [{ name: 'a.json', status: 'merged', message: 'merged' }, { name: 'b.json', status: 'skipped', message: 'same data' }],
                export: { format: 'dsd-net-export', now: 1_300_000, exported: 1_300_000, families: { dmr: family() } } });
    await done;
    await render();
    const bar = text('[data-test=filebar]');
    expect(bar).toContain('Viewing a merge of 2 files');
    expect(bar).toContain('b.json — same data');
    expect($$('[data-test=filebar] .st').map((e) => e.textContent)).toEqual(['merged', 'skipped']);
    expect($('[data-test=filebar] a[download]')).not.toBeNull();             // Save merged
  });

  it('opens details as a drawer on narrow screens; Esc closes it', async () => {
    (TestBed.inject(Breakpoints) as unknown as FakeBreakpoints).drawer.set(true);
    await serve();
    $$('app-calls-view app-radio a')[0].click();
    await render();
    expect(document.body.classList.contains('sheet')).toBe(true);
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    await render();
    expect(document.body.classList.contains('sheet')).toBe(false);
  });

  it('reports a lost connection', async () => {
    await serve();
    const p = page.poll();
    http().expectOne('/net.json').flush('', { status: 502, statusText: 'Bad Gateway' });
    await p;
    await render();
    expect(text('[data-test=live]')).toBe('disconnected — retrying');
    expect($$('app-calls-view tbody tr:not(.sub)').length).toBe(5);         // the data stays
  });
});
