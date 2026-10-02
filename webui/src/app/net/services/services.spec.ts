import { Component, inject } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { mb } from '../../core/format';
import { call } from '../logic/fixture.spec-helper';
import { FakeAudio, FakeWorker, LOCAL_ASR, explorerProviders, http, tick } from '../testing.spec-helper';
import { AsrService } from './asr';
import { Player } from './player';
import { Toast } from './toast';

@Component({ template: '', providers: [AsrService, Player, Toast] })
class Host {
  readonly asr = inject(AsrService);
  readonly player = inject(Player);
  readonly toast = inject(Toast);
}

describe('speech-to-text and the player', () => {
  let host: Host;
  beforeEach(() => {
    TestBed.configureTestingModule({ providers: explorerProviders() });
    host = TestBed.createComponent(Host).componentInstance;
  });

  const withAudio = call({ audio: 'call_1_ab_5.wav', audio_ms: 2600 });

  async function config(c = LOCAL_ASR) {
    http().expectOne('/net/asr/config.json').flush(c);
    await tick();
  }

  it('transcribes a call: loads the model, decodes the audio, runs the worker, keeps the cleaned text', async () => {
    const asr = host.asr;
    asr.transcribe(withAudio, false);
    await config();
    const w = FakeWorker.last!;
    expect(w.sent[0]).toMatchObject({ cmd: 'load', model: 'Xenova/whisper-small', local: '/net/asr/models/' });
    expect(String(w.sent[0]['lib'])).toMatch(/\/net\/asr\/transformers\.min\.js$/);
    expect(asr.describe('call_1_ab_5.wav', mb).text).toContain('Loading the speech model');
    w.reply({ type: 'progress', loaded: 3 * 1048576, total: 9e7 });
    expect(asr.describe('call_1_ab_5.wav', mb).text).toContain('3.0 MB');
    w.reply({ type: 'ready', threads: 4 });
    http().expectOne('/net/audio/call_1_ab_5.wav').flush(new ArrayBuffer(64));
    await tick();
    const run = w.sent[1];
    expect(run).toMatchObject({ cmd: 'run', language: 'english' });
    expect(asr.describe('call_1_ab_5.wav', mb).text).toBe('Transcribing…');
    w.reply({ type: 'result', id: run['id'], text: ' Seven, eight [noise] nine. ', ms: 6200 });
    const line = asr.describe('call_1_ab_5.wav', mb);
    expect(line.text).toBe('Seven, eight nine.');
    expect(line.meta).toBe('whisper-small · 6.2 s');
    expect(JSON.parse(localStorage.getItem('netx.asr.tx')!)['call_1_ab_5.wav'].t).toBe('Seven, eight nine.');
  });

  it("doesn't redo a finished transcript, and drops Whisper's inventions", async () => {
    const asr = host.asr;
    asr.transcribe(withAudio, false);
    await config();
    FakeWorker.last!.reply({ type: 'ready' });
    http().expectOne('/net/audio/call_1_ab_5.wav').flush(new ArrayBuffer(8));
    await tick();
    FakeWorker.last!.reply({ type: 'result', id: FakeWorker.last!.sent[1]['id'], text: 'Thanks for watching!', ms: 900 });
    expect(asr.describe('call_1_ab_5.wav', mb).text).toBe('(no clear speech recognized)');
    asr.transcribe(withAudio, false);
    await tick();
    expect(FakeWorker.last!.sent.length).toBe(2);         // no new run
  });

  it('keeps a live call\'s transcript only as partial', async () => {
    host.asr.transcribe(withAudio, true);
    await config();
    FakeWorker.last!.reply({ type: 'ready' });
    http().expectOne('/net/audio/call_1_ab_5.wav').flush(new ArrayBuffer(8));
    await tick();
    FakeWorker.last!.reply({ type: 'result', id: FakeWorker.last!.sent[1]['id'], text: 'Unit one', ms: 1 });
    expect(host.asr.describe('call_1_ab_5.wav', mb).meta).toContain('partial');
    expect(JSON.parse(localStorage.getItem('netx.asr.tx')!)['call_1_ab_5.wav']).toBeUndefined();
  });

  it('without files on the server, offers the internet -- and then uses the CDN', async () => {
    host.asr.transcribe(withAudio, false);
    await config({ local: false, lib: false, models: [], model: 'Xenova/whisper-small', language: 'english' });
    const line = host.asr.describe('call_1_ab_5.wav', mb);
    expect(line.offerInternet).toBe(true);
    expect(FakeWorker.last).toBeNull();
    host.asr.useInternet();
    expect(FakeWorker.last!.sent[0]).toMatchObject({ cmd: 'load', lib: expect.stringContaining('cdn.jsdelivr.net'), local: null });
  });

  it('reports a model that fails to load, and audio gone from the server', async () => {
    host.asr.transcribe(withAudio, false);
    await config();
    FakeWorker.last!.reply({ type: 'error', cmd: 'load', msg: 'no such model' });
    expect(host.asr.describe('call_1_ab_5.wav', mb).text).toBe('Speech-to-text failed: no such model');
    expect(FakeWorker.last!.terminated).toBe(true);

    host.asr.state.set('off');
    host.asr.transcribe(withAudio, false);
    await tick();
    FakeWorker.last!.reply({ type: 'ready' });
    http().expectOne('/net/audio/call_1_ab_5.wav').flush(new ArrayBuffer(0), { status: 404, statusText: 'Not Found' });
    await tick();
    expect(host.asr.describe('call_1_ab_5.wav', mb).text).toBe('Could not transcribe this call: its audio is no longer on the server');
  });

  it('chooses the model and language; English-only models get no language', async () => {
    const asr = host.asr;
    void asr.loadConfig();
    await config();
    expect(asr.models()).toEqual(['Xenova/whisper-base', 'Xenova/whisper-small']);
    asr.setModel('Xenova/whisper-tiny');                  // not on the server: falls back
    expect(asr.currentModel()).toBe('Xenova/whisper-small');
    asr.setModel('Xenova/whisper-base');
    asr.setLang('italian');
    expect([asr.currentModel(), asr.currentLang()]).toEqual(['Xenova/whisper-base', 'italian']);
    expect(localStorage.getItem('netx.asr.lang')).toBe('italian');
    asr.setOn(false);
    expect(asr.describe('x', mb).text).toContain('off');
  });

  it('plays a call, transcribing it when on; playing it again stops it', async () => {
    const p = host.player;
    p.toggle(withAudio, false);
    expect(p.playing()).toBe('call_1_ab_5.wav');
    expect(p.call()).toBe(withAudio);
    expect(p.duration()).toBeCloseTo(2.6);
    http().expectOne('/net/asr/config.json');            // transcription asked for
    p.toggle(withAudio, false);
    expect(p.playing()).toBeNull();
    expect(p.call()).toBe(withAudio);                    // the bar stays
    p.close();
    expect(p.call()).toBeNull();
  });

  it('follows the audio element: progress, end, and a missing file', () => {
    host.asr.setOn(false);
    const p = host.player;
    p.toggle(withAudio, false);
    const a = (p as unknown as { audio: FakeAudio }).audio;
    expect(a.src).toBe('/net/audio/call_1_ab_5.wav');
    a.currentTime = 1.3; a.duration = 2.64;
    a.dispatchEvent(new Event('timeupdate'));
    expect([p.time(), p.duration()]).toEqual([1.3, 2.64]);
    a.dispatchEvent(new Event('ended'));
    expect(p.playing()).toBeNull();
    p.toggle(withAudio, false);
    a.dispatchEvent(new Event('error'));
    expect(host.toast.text()).toBe("This call's audio is no longer available.");
  });

  it('ignores calls without audio', () => {
    host.player.toggle(call({}), false);
    expect(host.player.call()).toBeNull();
  });
});
