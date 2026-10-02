import { Injectable, signal } from '@angular/core';

function mq(query: string) {
  const m = typeof window !== 'undefined' && window.matchMedia ? window.matchMedia(query) : null;
  const s = signal(!!m?.matches);
  m?.addEventListener?.('change', (e) => s.set(e.matches));
  return s.asReadonly();
}

/** The page's layout breakpoints, as signals (same queries as the CSS). */
@Injectable({ providedIn: 'root' })
export class Breakpoints {
  /** Phones: call cards, stacked tables, bottom-sheet details. */
  readonly phone = mq('(max-width: 640px)');
  /** Details open in a drawer instead of beside the tables. */
  readonly drawer = mq('(max-width: 1050px)');
  readonly mid = mq('(max-width: 900px)');
  /** Touch screens: tooltips show on tap. */
  readonly coarse = mq('(pointer: coarse)');
}
