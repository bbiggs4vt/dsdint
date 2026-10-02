import { NetCall } from '../../core/models';
import { callFile, callKey, callRate, heldCalls, isLive, safeName } from './calls';
import { exportProblem, isRecording, sourceText } from './export-file';
import { ALL, filterCalls, filterRadios, filterTalkgroups, matchesQuery, sortRows } from './filters';
import { call, family } from './fixture.spec-helper';
import {
  busiestOn, communities, communityNetworks, crossNetwork, hubRadios, linkedTalkgroups, ranked, recentCalls,
  sharedTalkgroupRadios,
} from './links';
import { NetColors, PALETTE, UNIDENTIFIED_COLOR, aliasOf, buildIndex, primaryNet } from './model-index';
import { cleanTranscript } from './transcript';
import { crc32, csv, csvCell, zipBytes } from './zip';

describe('model index', () => {
  const ix = buildIndex(family());

  it('indexes networks (oldest first), talkgroups and radios', () => {
    expect(ix.nets.map((n) => n.key)).toEqual(['net:9', 'cc:1@434425000']);
    expect(ix.tgById['10'].calls).toBe(3);
    expect(ix.rById['300'].peers).toEqual({ '400': 2 });
    expect(ix.netTg).toEqual({ 'net:9': 2, 'cc:1@434425000': 1 });
    expect(ix.netRad['net:9']).toBe(4);
  });

  it('ranks networks by calls for the primary network', () => {
    expect(ix.netRank['net:9']).toBe(0);
    expect(primaryNet(ix, ['cc:1@434425000', 'net:9'])).toBe('net:9');
    expect(primaryNet(ix, [])).toBeNull();
  });

  it('gives the latest alias', () => {
    expect(aliasOf(ix, '300')).toBe('UNIT 21B');
    expect(aliasOf(ix, '400')).toBe('');
    expect(aliasOf(ix, 'nope')).toBe('');
  });

  it('hands out stable network colours, grey for unidentified', () => {
    const c = new NetColors();
    c.assign('dmr', ix);
    expect(c.colorFor('dmr', ix.netByKey['net:9'])).toBe(PALETTE[0]);
    expect(c.colorFor('dmr', ix.netByKey['cc:1@434425000'])).toBe(PALETTE[1]);
    expect(c.colorFor('p25', ix.netByKey['cc:1@434425000'])).toBe(PALETTE[0]);   // per protocol
    expect(c.colorFor('dmr', { ...ix.nets[0], key: 'x', confidence: 'none' })).toBe(UNIDENTIFIED_COLOR);
    expect(c.colorFor('dmr', undefined)).toBe(UNIDENTIFIED_COLOR);
  });
});

describe('filters', () => {
  const ix = buildIndex(family());

  it('matches case-insensitively, in arrays too', () => {
    expect(matchesQuery('reyes', 'K. REYES')).toBe(true);
    expect(matchesQuery('21b', null, ['UNIT 21', 'UNIT 21B'])).toBe(true);
    expect(matchesQuery('zzz', 'a', ['b'], undefined)).toBe(false);
  });

  it('filters calls by network, search, MHz, radio alias and audio', () => {
    const ids = (cs: NetCall[]) => cs.map((c) => c.id);
    expect(ids(filterCalls(ix, { net: ALL, q: '' }))).toEqual([5, 4, 3, 2, 1]);
    expect(ids(filterCalls(ix, { net: 'cc:1@434425000', q: '' }))).toEqual([2]);
    expect(ids(filterCalls(ix, { net: ALL, q: 'route' }))).toEqual([2]);
    expect(ids(filterCalls(ix, { net: ALL, q: '434.42' }))).toEqual([2]);
    expect(ids(filterCalls(ix, { net: ALL, q: 'unit 21' }))).toEqual([5]);   // the radio's alias
    expect(ids(filterCalls(ix, { net: ALL, q: '', audioOnly: true }))).toEqual([5]);
  });

  it('filters talkgroups and radios', () => {
    expect(filterTalkgroups(ix, { net: 'cc:1@434425000', q: '' }).map((t) => t.id)).toEqual(['9']);
    expect(filterTalkgroups(ix, { net: ALL, q: '10' }).map((t) => t.id)).toEqual(['10']);
    expect(filterRadios(ix, { net: ALL, q: 'reyes' }).map((r) => r.id)).toEqual(['100']);
    expect(filterRadios(ix, { net: 'cc:1@434425000', q: '' }).map((r) => r.id)).toEqual(['200']);
  });

  it('sorts naturally and stably', () => {
    const rows = [{ id: 'TG 10' }, { id: 'TG 9' }, { id: 'TG 100' }];
    expect(sortRows(rows, (r) => r.id, 1).map((r) => r.id)).toEqual(['TG 9', 'TG 10', 'TG 100']);
    expect(sortRows([3, 1, 2], (n) => n, -1)).toEqual([3, 2, 1]);
    const tied = [{ k: 1, n: 'a' }, { k: 0, n: 'b' }, { k: 1, n: 'c' }];
    expect(sortRows(tied, (r) => r.k, -1).map((r) => r.n)).toEqual(['a', 'c', 'b']);
  });
});

describe('calls', () => {
  it('knows when a call is live', () => {
    const c = call({ open: true, last: 1000 });
    expect(isLive(c, 3500, false)).toBe(true);
    expect(isLive(c, 4500, false)).toBe(false);
    expect(isLive(c, 3500, true)).toBe(false);          // a file view is never live
    expect(isLive({ ...c, open: false }, 1500, false)).toBe(false);
  });

  it('names audio files after the call', () => {
    const t = Date.UTC(2026, 9, 2, 17, 29, 31);
    expect(callFile(call({ start: t, freq: 434425000, slot: '1', tgt: '1', src: '123' })))
      .toBe('call_20261002T172931Z_434.4250MHz_s1_TG1_from_123.wav');
    expect(callFile(call({ start: t, freq: 0, slot: '', priv: true, tgt: 'N0CALL/P', src: '' })))
      .toBe('call_20261002T172931Z_to_N0CALL_P_from_x.wav');
    expect(safeName('a b/c')).toBe('a_b_c');
  });

  it('keys calls by start, stream, slot and parties', () => {
    expect(callKey(call({ start: 5, session: 2, slot: '1', src: 'a', tgt: 'b' }))).toBe('5|2|1|a|b');
  });

  it('uses the server rate unless filtered', () => {
    const r = callRate({ per_s_1m: 0.25, per_s_10m: 0.1, total: 42 }, false, [], 0, false, false);
    expect(r.text).toBe('0.3');
    expect(r.tip).toContain('42 calls counted');
    const now = 1_000_000;
    const cs = [call({ start: now - 10_000 }), call({ start: now - 20_000 }), call({ start: now - 120_000 })];
    const f = callRate(undefined, true, cs, now, true, true);
    expect(f.text).toBe('0.0');                           // 2 calls over 60 s
    expect(f.tip).toContain('on this network matching the search: 2 calls');
    expect(callRate(undefined, true, [call({ start: now - 10_000 })], now, false, false).text).toBe('0.1');
  });

  it('holds the paused list and counts new calls', () => {
    const a = call({ id: 1, start: 1 }), b = call({ id: 2, start: 2 });
    const held = [a, b], keysHeld = new Set(held.map(callKey));
    const bNow = { ...b, last: 99, audio: 'x.wav' };
    const c = call({ id: 3, start: 3 });
    const r = heldCalls(held, keysHeld, [c, bNow], (cs) => cs);
    expect(r.list).toEqual([a, bNow]);                    // a has rolled off: kept as it was
    expect(r.fresh).toBe(1);
  });
});

describe('links and details', () => {
  const ix = buildIndex(family());

  it('finds talk communities', () => {
    const cs = communities(ix.tgs, ix.radios);
    expect(cs.length).toBe(1);                            // 400 joins through its private calls with 300
    expect(cs[0].r.sort()).toEqual(['100', '200', '300', '400']);
    expect(cs[0].t.sort()).toEqual(['10', '9']);
  });

  it('keeps separate groups apart and drops a lone talkgroup', () => {
    const F = family();
    F.radios.push({ id: '900', aliases: [], tgs: { '77': 1 }, peers: {}, networks: [], calls: 1, first: 0, last: 0 });
    F.talkgroups.push({ id: '77', networks: [], radios: { '900': 1 }, calls: 1, emerg: 0, enc: 0, first: 0, last: 0 },
                      { id: '88', networks: [], radios: {}, calls: 1, emerg: 0, enc: 0, first: 0, last: 0 });
    const cs = communities(F.talkgroups, F.radios);
    expect(cs.map((c) => c.r.length + c.t.length)).toEqual([6, 2]);
    expect(cs[1]).toEqual({ t: ['77'], r: ['900'] });
  });

  it('joins private-call partners into one community', () => {
    const F = family();
    const cs = communities(F.talkgroups, F.radios);
    const withPeers = cs.find((c) => c.r.includes('300'))!;
    expect(withPeers.r).toContain('400');
  });

  it('lists a community\'s networks, hubs and cross-network entities', () => {
    const c = communities(ix.tgs, ix.radios)[0];
    expect(communityNetworks(c, ix).sort()).toEqual(['cc:1@434425000', 'net:9']);
    expect(hubRadios(ix.radios)).toEqual([]);
    const x = crossNetwork(ix.tgs, ix.radios);
    expect(x.tgs.map((t) => t.id)).toEqual(['9']);
    expect(x.radios.map((r) => r.id)).toEqual(['200']);
  });

  it('ranks counts, linked talkgroups and shared radios', () => {
    expect(ranked({ a: 1, b: 3 })).toEqual([{ id: 'b', n: 3 }, { id: 'a', n: 1 }]);
    expect(ranked(undefined)).toEqual([]);
    expect(linkedTalkgroups(ix.tgById['9'], ix)).toEqual([{ id: '10', n: 1 }]);
    expect(sharedTalkgroupRadios(ix.rById['200'], ix)).toEqual([{ id: '100', n: 1 }, { id: '300', n: 1 }]);
  });

  it('lists a network\'s busiest talkgroups and radios', () => {
    const b = busiestOn(ix.netByKey['cc:1@434425000'], ix);
    expect(b.tgs).toEqual([{ id: '9', n: 4 }]);
    expect(b.radios).toEqual([{ id: '200', n: 3 }]);
  });

  it('lists recent calls of a talkgroup or radio', () => {
    expect(recentCalls(ix, { type: 'tg', id: '9' }).map((c) => c.id)).toEqual([4, 2, 1]);
    expect(recentCalls(ix, { type: 'radio', id: '400' }).map((c) => c.id)).toEqual([5]);   // as the private target
    expect(recentCalls(ix, { type: 'tg', id: '9' }, 1).length).toBe(1);
  });
});

describe('transcripts', () => {
  it('keeps speech, drops Whisper\'s inventions and loops', () => {
    expect(cleanTranscript('Seven, eight, nine, ten.')).toBe('Seven, eight, nine, ten.');
    expect(cleanTranscript('  Copy  [music] that ')).toBe('Copy that');
    expect(cleanTranscript('you')).toBe('');
    expect(cleanTranscript('Thanks for watching!')).toBe('');
    expect(cleanTranscript("So, I mean, I'm telling you, I'm telling you, I'm telling you, I'm telling you, I'm")).toBe('');
    expect(cleanTranscript("that's that's that's that's")).toBe('');
    expect(cleanTranscript('Go go go now')).toBe('Go go go now');   // three is speech
    expect(cleanTranscript('...')).toBe('');
    expect(cleanTranscript(null)).toBe('');
    expect(cleanTranscript('E\' un\'altra connessione')).toBe('E\' un\'altra connessione');
  });
});

describe('zip and csv', () => {
  it('computes CRC-32', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array())).toBe(0);
  });

  it('writes a valid stored zip', () => {
    const a = new TextEncoder().encode('hello'), b = new Uint8Array([1, 2, 3]);
    const z = zipBytes([{ name: 'a.txt', data: a }, { name: 'ü.bin', data: b }], new Date(2026, 9, 2, 12, 0, 0));
    const v = new DataView(z.buffer);
    expect(v.getUint32(0, true)).toBe(0x04034b50);
    expect(v.getUint32(14, true)).toBe(crc32(a));
    expect(new TextDecoder().decode(z.subarray(30, 35))).toBe('a.txt');
    expect(new TextDecoder().decode(z.subarray(35, 40))).toBe('hello');
    const end = z.length - 22;
    expect(v.getUint32(end, true)).toBe(0x06054b50);
    expect(v.getUint16(end + 10, true)).toBe(2);           // entries
    const cdOff = v.getUint32(end + 16, true);
    expect(v.getUint32(cdOff, true)).toBe(0x02014b50);
    expect(cdOff).toBe(30 + 5 + 5 + 30 + 6 + 3);           // after both local entries ("ü" is 2 bytes)
    const second = cdOff + 46 + 5;                          // 2nd central record
    expect(v.getUint32(second + 42, true)).toBe(30 + 5 + 5);   // its local header offset
  });

  it('quotes and defuses CSV cells', () => {
    expect(csvCell('plain')).toBe('plain');
    expect(csvCell('a,b')).toBe('"a,b"');
    expect(csvCell('say "hi"')).toBe('"say ""hi"""');
    expect(csvCell('=SUM(A1)')).toBe("'=SUM(A1)");
    expect(csvCell(null)).toBe('');
    expect(csv([['a', 1], ['b,c', '']])).toBe('﻿a,1\r\n"b,c",\r\n');
  });
});

describe('export files', () => {
  it('recognises exports and recordings', () => {
    expect(exportProblem({ format: 'dsd-net-export', families: {} }, 'x.json')).toBeNull();
    expect(exportProblem({ now: 5, families: {} }, 'x.json')).toBeNull();
    expect(exportProblem({ hello: 1 }, 'x.json')).toContain('"x.json" is not');
    expect(exportProblem(null, 'x.json')).not.toBeNull();
    expect(isRecording('{"op":"header","v":1}')).toBe(true);
    expect(isRecording('{"format":"dsd-net-export"}')).toBe(false);
  });

  it('describes a source', () => {
    const dt = (ms: number) => new Date(ms).toISOString().replace('T', ' ').slice(0, 19) + 'Z';
    expect(sourceText({ name: 'rx-north', since: Date.UTC(2026, 9, 2, 7, 0), through: Date.UTC(2026, 9, 2, 7, 30) }, dt))
      .toBe('rx-north 10-02 07:00–07:30Z');
    expect(sourceText({ instance: 'abcdef0123' }, dt)).toBe('abcdef01 start–Z');
  });
});
