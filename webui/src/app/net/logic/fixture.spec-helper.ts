import { NetCall, NetDoc, NetFamily } from '../../core/models';

/** A call with defaults. */
export function call(over: Partial<NetCall>): NetCall {
  return {
    id: 1, session: 1, net: 'net:9', site: '', freq: 460025000, slot: '1', src: '100', tgt: '9', alias: '', text: '',
    priv: false, voice: true, data: false, emerg: false, enc: false, open: false, streams: 1,
    start: 1_000_000, last: 1_002_000, ...over,
  };
}

/**
 * A small DMR family: a strong network (net:9) with TG 9 and TG 10, a channel
 * network (cc:1@...) with TG 9 too; radios 100, 200 (on both networks), 300
 * and a private pair 300 <-> 400.
 */
export function family(): NetFamily {
  return {
    networks: [
      { key: 'cc:1@434425000', label: 'Color Code 1 · 434.4250 MHz', confidence: 'channel', ids: { cc: '1' }, sites: [],
        freqs: [434425000], sessions: 1, calls: 2, first: 900_000, last: 1_200_000 },
      { key: 'net:9', label: 'Network 9', confidence: 'strong', ids: { network_id: '9' }, sites: ['Site 1'],
        freqs: [460025000], sessions: 2, calls: 5, first: 800_000, last: 1_300_000 },
    ],
    talkgroups: [
      { id: '9', networks: ['net:9', 'cc:1@434425000'], radios: { '100': 3, '200': 1 }, calls: 4, emerg: 1, enc: 0, first: 800_000, last: 1_300_000 },
      { id: '10', networks: ['net:9'], radios: { '200': 2, '300': 1 }, calls: 3, emerg: 0, enc: 1, first: 850_000, last: 1_250_000 },
    ],
    radios: [
      { id: '100', aliases: ['K. REYES'], tgs: { '9': 3 }, peers: {}, networks: ['net:9'], calls: 3, first: 800_000, last: 1_300_000 },
      { id: '200', aliases: [], tgs: { '9': 1, '10': 2 }, peers: {}, networks: ['net:9', 'cc:1@434425000'], calls: 3, first: 820_000, last: 1_250_000 },
      { id: '300', aliases: ['UNIT 21', 'UNIT 21B'], tgs: { '10': 1 }, peers: { '400': 2 }, networks: ['net:9'], calls: 3, first: 830_000, last: 1_200_000 },
      { id: '400', aliases: [], tgs: {}, peers: { '300': 2 }, networks: ['net:9'], calls: 0, first: 830_000, last: 1_200_000 },
    ],
    calls: [
      call({ id: 5, src: '300', tgt: '400', priv: true, start: 1_290_000, last: 1_291_000, audio: 'call_1290000_ab_5.wav', audio_ms: 2600 }),
      call({ id: 4, src: '100', tgt: '9', alias: 'K. REYES', start: 1_280_000, last: 1_299_000, open: true }),
      call({ id: 3, src: '200', tgt: '10', slot: '2', start: 1_270_000, last: 1_271_000, enc: true }),
      call({ id: 2, src: '200', tgt: '9', net: 'cc:1@434425000', freq: 434425000, start: 1_100_000, last: 1_101_000, text: 'en route' }),
      call({ id: 1, src: '100', tgt: '9', start: 1_000_000, last: 1_002_000, emerg: true }),
    ],
  };
}

export function netDoc(over: Partial<NetDoc> = {}): NetDoc {
  return { version: 7, now: 1_300_000, rec: {}, audio: {}, max_calls: 5000, imports: [],
           rates: { dmr: { per_s_1m: 0.25, per_s_10m: 0.1, total: 42 } }, families: { dmr: family() }, ...over };
}
