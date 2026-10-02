import { NgTemplateOutlet } from '@angular/common';
import { Component, computed, inject, input, ChangeDetectionStrategy } from '@angular/core';
import { ago, hms, keys, mhz } from '../../core/format';
import { NetCall } from '../../core/models';
import { isLive } from '../logic/calls';
import { ALL } from '../logic/filters';
import { Ranked, busiestOn, linkedTalkgroups, ranked, recentCalls, sharedTalkgroupRadios } from '../logic/links';
import { NetStore } from '../state/net-store';
import { CallAudio, ConfidenceBadge, NetChip, RadioLink, Swatch, TgLink, Transcript } from './widgets';

/** A ranked list with bars: radios / talkgroups and their counts. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-rank-list',
  imports: [RadioLink, TgLink],
  template: `@if (items().length) {
    <ul class="lst">
      @for (it of items().slice(0, max()); track it.id) {
        <li><div>@if (kind() === 'tg') {<app-tg [id]="it.id" />} @else {<app-radio [id]="it.id" />}
          <div class="bar" [style.width.%]="width(it)"></div></div>
          <span class="c">{{ label() ? label()!(it) : it.n }}</span></li>
      }
      @if (items().length > max()) {<li><span class="alias">+{{ items().length - max() }} more</span><span></span></li>}
    </ul>
  } @else {<div class="hint">None.</div>}`,
})
export class RankList {
  readonly items = input.required<Ranked[]>();
  readonly kind = input.required<'tg' | 'radio'>();
  readonly max = input(25);
  readonly label = input<((it: Ranked) => string) | null>(null);
  width(it: Ranked): number {
    const top = this.items()[0]?.n || 1;
    return Math.max(4, 100 * it.n / top);
  }
}

/** The details of the selected radio, talkgroup or network. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-details',
  imports: [NgTemplateOutlet, RankList, RadioLink, TgLink, NetChip, Swatch, ConfidenceBadge, CallAudio, Transcript],
  templateUrl: './details-panel.html',
})
export class DetailsPanel {
  protected readonly store = inject(NetStore);
  /** Shown as a drawer (narrow screens): a Close button. */
  readonly drawer = input(false);
  protected readonly ago = ago;
  protected readonly hms = hms;
  protected readonly keys = keys;
  protected readonly mhz = mhz;
  readonly ix = this.store.ix;
  readonly sel = this.store.sel;

  readonly tg = computed(() => { const s = this.sel(); return s?.type === 'tg' ? this.ix()?.tgById[s.id] ?? null : null; });
  readonly radio = computed(() => { const s = this.sel(); return s?.type === 'radio' ? this.ix()?.rById[s.id] ?? null : null; });
  readonly net = computed(() => { const s = this.sel(); return s?.type === 'net' ? this.ix()?.netByKey[s.id] ?? null : null; });

  readonly tgRadios = computed(() => ranked(this.tg()?.radios));
  readonly tgLinked = computed(() => (this.tg() && this.ix() ? linkedTalkgroups(this.tg()!, this.ix()!) : []));
  readonly radioTgs = computed(() => ranked(this.radio()?.tgs));
  readonly radioPeers = computed(() => ranked(this.radio()?.peers));
  readonly radioShares = computed(() => (this.radio() && this.ix() ? sharedTalkgroupRadios(this.radio()!, this.ix()!) : []));
  readonly busiest = computed(() => (this.net() && this.ix() ? busiestOn(this.net()!, this.ix()!) : { tgs: [], radios: [] }));
  readonly recent = computed(() => {
    const s = this.sel(), ix = this.ix();
    return s && ix && s.type !== 'net' ? recentCalls(ix, { type: s.type, id: s.id }) : [];
  });
  readonly netOnly = computed(() => this.store.activeNet() === this.net()?.key);

  readonly sharedLabel = (it: Ranked) => it.n + ' shared';
  readonly tgsLabel = (it: Ranked) => it.n + ' TG' + (it.n > 1 ? 's' : '');

  live(c: NetCall): boolean {
    return isLive(c, this.store.now(), this.store.fileMode());
  }
  ids(o: Record<string, string>): string {
    return keys(o).map((k) => k + '=' + o[k]).join('   ') || '—';
  }
  channels(f: number[] | undefined): string {
    return (f || []).map((x) => mhz(x) + ' MHz').join(', ') || 'Unknown (the client sent no center_freq).';
  }
  toggleNetFilter(key: string): void {
    this.store.setNet(this.store.activeNet() === key ? ALL : key);
  }
  close(): void {
    this.store.closeSheet();
  }
}
