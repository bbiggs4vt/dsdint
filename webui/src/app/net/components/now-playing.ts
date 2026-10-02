import { Component, computed, inject, ChangeDetectionStrategy } from '@angular/core';
import { hms, mb, mhz, mmss } from '../../core/format';
import { NetApi } from '../../core/net-api.service';
import { callFile, isLive } from '../logic/calls';
import { AsrService } from '../services/asr';
import { Player } from '../services/player';
import { NetStore } from '../state/net-store';
import { CallTo, NetChip, RadioLink } from './widgets';

/**
 * The call playing (or last played), pinned to the bottom of the window: when,
 * channel, who -> whom, progress, download, and its transcript (or what
 * speech-to-text is doing).
 */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-now-playing',
  imports: [RadioLink, NetChip, CallTo],
  template: `@if (call(); as c) {
  <div class="np" data-test="np">
    <div class="prog" [style.width.%]="progress()"></div>
    <div class="in">
      <button class="play" type="button" title="Play / stop" [class.on]="!!player.playing()" (click)="replay()"><span>{{ player.playing() ? '■' : '▶' }}</span></button>
      <div class="main">
        <div class="meta">
          <b class="mono">{{ hms(c.start) }}Z</b>
          @if (c.freq) {<span class="mono">{{ mhz(c.freq) }} MHz</span>}
          @if (c.slot) {<span>slot {{ c.slot }}</span>}
          <span>@if (c.src) {<app-radio [id]="c.src" [alias]="c.alias" />} @else {?} → <app-call-to [c]="c" /></span>
          @if (c.net) {<app-net [key]="c.net" />}
          <span class="mono">{{ times() }}</span>
        </div>
        <div class="tx" [class.st]="line().muted" data-test="np-tx">{{ line().text }}@if (line().meta) {<span class="alias">  · {{ line().meta }}</span>}@if (line().offerInternet) {
          <a href="#" (click)="internet($event)">load them from the internet</a> (jsDelivr and Hugging Face, ~100 MB, then cached).}</div>
      </div>
      <a class="dl" [href]="url()" [attr.download]="file()" title="Download this call's audio">⤓</a>
      <button class="x" type="button" title="Close" aria-label="Close" (click)="player.close()">✕</button>
    </div>
  </div>
}`,
})
export class NowPlaying {
  protected readonly player = inject(Player);
  private readonly asr = inject(AsrService);
  private readonly store = inject(NetStore);
  protected readonly hms = hms;
  protected readonly mhz = mhz;
  readonly call = this.player.call;
  readonly progress = computed(() => {
    const d = this.player.duration();
    return d ? Math.min(100, 100 * this.player.time() / d) : 0;
  });
  readonly times = computed(() => mmss(this.player.time() * 1000) + ' / ' + mmss(this.player.duration() * 1000));
  readonly line = computed(() => this.asr.describe(this.call()?.audio || '', mb));
  readonly url = computed(() => NetApi.audioUrl(this.call()?.audio || ''));
  readonly file = computed(() => (this.call() ? callFile(this.call()!) : ''));

  replay(): void {
    const c = this.call();
    if (c) this.player.toggle(c, isLive(c, this.store.now(), this.store.fileMode()));
  }
  internet(e: Event): void {
    e.preventDefault();
    this.asr.useInternet();
  }
}
