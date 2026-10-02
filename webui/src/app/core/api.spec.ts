import { provideHttpClient, withFetch } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { TestBed } from '@angular/core/testing';
import { NetApi } from './net-api.service';
import { StatusApi } from './status-api.service';

describe('API services', () => {
  let http: HttpTestingController;
  beforeEach(() => {
    TestBed.configureTestingModule({ providers: [provideHttpClient(withFetch()), provideHttpClientTesting()] });
    http = TestBed.inject(HttpTestingController);
  });
  afterEach(() => http.verify());

  it('status endpoints', () => {
    const api = TestBed.inject(StatusApi);
    api.status().subscribe();
    api.log().subscribe();
    api.clearLog().subscribe();
    let iq: boolean | undefined;
    api.setIqLogging(true).subscribe((r) => (iq = r.iq_log_enabled));
    expect(http.expectOne('/status.json').request.method).toBe('GET');
    http.expectOne('/log.json');
    http.expectOne('/log/clear');
    http.expectOne('/iq_log/on').flush({ iq_log_enabled: true });
    expect(iq).toBe(true);
  });

  it('explorer endpoints', () => {
    const api = TestBed.inject(NetApi);
    api.net().subscribe();
    api.recording(true, true).subscribe();
    api.recording(false).subscribe();
    api.audio(true).subscribe();
    api.audio(false).subscribe();
    api.clear().subscribe();
    api.removeImport(3).subscribe();
    api.clearImports().subscribe();
    api.asrConfig().subscribe();
    for (const u of ['/net.json', '/net/log/on?clear=1', '/net/log/off', '/net/audio/on', '/net/audio/off',
                     '/net/clear', '/net/imports/remove?id=3', '/net/imports/clear', '/net/asr/config.json'])
      http.expectOne(u);
  });

  it('posts imports and merges', () => {
    const api = TestBed.inject(NetApi);
    api.importExport('rx north.json', new Blob(['{}'])).subscribe();
    api.merge([{ name: 'a.json', text: '{}' }]).subscribe();
    const imp = http.expectOne('/net/import?name=rx%20north.json');
    expect(imp.request.method).toBe('POST');
    const m = http.expectOne('/net/merge');
    expect(m.request.body).toEqual({ files: [{ name: 'a.json', text: '{}' }] });
  });

  it('builds audio URLs safely', () => {
    expect(NetApi.audioUrl('call_1_ab_2.wav')).toBe('/net/audio/call_1_ab_2.wav');
    expect(NetApi.audioUrl('../x')).toBe('/net/audio/..%2Fx');
  });
});
