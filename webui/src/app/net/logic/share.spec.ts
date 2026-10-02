import { call, netDoc } from './fixture.spec-helper';
import { reuse, shareUnchanged } from './share';

describe('sharing unchanged data between polls', () => {
  it('keeps equal elements, takes changed and new ones', () => {
    const a = [{ id: 'x', n: 1, m: { a: 1 } }, { id: 'y', n: 2, m: {} }];
    const b = [{ id: 'z', n: 0, m: {} }, { id: 'x', n: 1, m: { a: 1 } }, { id: 'y', n: 3, m: {} }];
    const r = reuse(a, b, (e) => e.id);
    expect(r[0]).toBe(b[0]);
    expect(r[1]).toBe(a[0]);                                 // unchanged: the old object
    expect(r[2]).toBe(b[2]);                                 // changed
  });

  it('returns the old list itself when nothing changed', () => {
    const a = [{ id: 'x', n: 1 }];
    expect(reuse(a, [{ id: 'x', n: 1 }], (e) => e.id)).toBe(a);
    expect(reuse([], [{ id: 'x', n: 1 }], (e) => e.id)).toEqual([{ id: 'x', n: 1 }]);
  });

  it('keeps a whole family when nothing in it changed', () => {
    const a = netDoc(), b = netDoc({ version: 8, now: 1_301_000 });
    const s = shareUnchanged(a, b);
    expect(s.families['dmr']).toBe(a.families['dmr']);
    expect(s.now).toBe(1_301_000);
  });

  it('replaces only the call that changed', () => {
    const a = netDoc(), b = netDoc();
    b.families['dmr'].calls[1] = { ...b.families['dmr'].calls[1], last: 1_300_500 };
    b.families['dmr'].calls.unshift(call({ id: 6, start: 1_300_100 }));
    const s = shareUnchanged(a, b).families['dmr'];
    expect(s.calls[0]).toBe(b.families['dmr'].calls[0]);
    expect(s.calls[2]).toBe(b.families['dmr'].calls[2]);   // changed
    expect(s.calls[3]).toBe(a.families['dmr'].calls[2]);   // unchanged
    expect(s.radios).toBe(a.families['dmr'].radios);
    expect(shareUnchanged(null, b)).toBe(b);
  });
});
