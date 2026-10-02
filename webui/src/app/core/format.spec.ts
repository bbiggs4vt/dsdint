import { ago, dt, dur, hms, humanDuration, keys, mb, mhz, mmss, protocolTrail, stamp } from './format';

describe('format', () => {
  const t = Date.UTC(2026, 9, 2, 7, 4, 9);   // 2026-10-02 07:04:09Z

  it('formats UTC times', () => {
    expect(hms(t)).toBe('07:04:09');
    expect(dt(t)).toBe('2026-10-02 07:04:09Z');
    expect(stamp(t)).toBe('20261002T070409Z');
  });

  it('says how long ago', () => {
    expect(ago(0, t)).toBe('-');
    expect(ago(t - 12_000, t)).toBe('12s ago');
    expect(ago(t - 3 * 60_000, t)).toBe('3m ago');
    expect(ago(t - 2 * 3_600_000, t)).toBe('2h ago');
    expect(ago(t - 4 * 86_400_000, t)).toBe('4d ago');
    expect(ago(t + 5000, t)).toBe('0s ago');   // clock skew never goes negative
  });

  it('formats call durations', () => {
    expect(dur(0)).toBe('<1s');
    expect(dur(-5)).toBe('<1s');
    expect(dur(2400)).toBe('2.4s');
    expect(dur(37_200)).toBe('37s');
    expect(dur(185_000)).toBe('3m 05s');
  });

  it('formats the status page durations', () => {
    expect(humanDuration(45)).toBe('45s');
    expect(humanDuration(23 * 60 + 45)).toBe('23m 45s');
    expect(humanDuration(3600 + 23 * 60 + 45)).toBe('1h 23m 45s');
    expect(humanDuration(3600)).toBe('1h 0m 0s');
    expect(humanDuration(-3)).toBe('0s');
  });

  it('formats audio lengths and sizes', () => {
    expect(mmss(7_400)).toBe('0:07');
    expect(mmss(65_000)).toBe('1:05');
    expect(mb(3 * 1048576)).toBe('3.0 MB');
    expect(mb(312 * 1024)).toBe('312 KB');
    expect(mb(10)).toBe('1 KB');
  });

  it('formats frequencies with at least 4 decimals', () => {
    expect(mhz(460025000)).toBe('460.0250');
    expect(mhz(851012500)).toBe('851.0125');
    expect(mhz(434423750)).toBe('434.42375');
    expect(mhz(0)).toBe('');
    expect(mhz(undefined)).toBe('');
  });

  it('builds the protocol trail', () => {
    expect(protocolTrail(['dmr', 'nxdn48', 'pager-auto'], ['dmr', 'pager-auto'], 'pager-auto'))
      .toBe('dmr → nxdn48 ✗ → pager-auto');
    expect(protocolTrail([], ['p25p1'], 'p25p1')).toBe('p25p1');
    expect(protocolTrail(undefined, undefined, '')).toBe('-');
    expect(protocolTrail([], [], 'dmr')).toBe('dmr');
  });

  it('lists keys of possibly-missing objects', () => {
    expect(keys({ a: 1, b: 2 })).toEqual(['a', 'b']);
    expect(keys(undefined)).toEqual([]);
  });
});
