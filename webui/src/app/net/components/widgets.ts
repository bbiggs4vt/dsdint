import { Component, computed, inject, input, ChangeDetectionStrategy } from '@angular/core';
import { dur, mhz, mmss } from '../../core/format';
import { Confidence, NetCall } from '../../core/models';
import { NetApi } from '../../core/net-api.service';
import { callFile, isLive } from '../logic/calls';
import { aliasOf } from '../logic/model-index';
import { NetStore } from '../state/net-store';
import { Breakpoints } from '../services/breakpoints';
import { AsrService } from '../services/asr';
import { Player } from '../services/player';

/** A radio id (with its alias), opening its details. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-radio',
  template: `<a class="ent" href="#" [title]="'Radio ' + id()" (click)="open($event)">{{ id() }}@if (shownAlias()) {<span class="alias"> {{ shownAlias() }}</span>}</a>`,
})
export class RadioLink {
  private readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  readonly id = input.required<string>();
  /** The alias to show (a call's); empty = the radio's latest. */
  readonly alias = input<string | null | undefined>(undefined);
  readonly shownAlias = computed(() => {
    const a = this.alias(), ix = this.store.ix();
    return a || (ix ? aliasOf(ix, this.id()) : '');
  });
  open(e: Event): void {
    e.preventDefault();
    e.stopPropagation();
    this.store.select('radio', this.id(), this.bp.drawer());
  }
}

/** "TG 1234", opening the talkgroup's details. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-tg',
  template: `<a class="ent" href="#" [title]="'Talkgroup ' + id()" (click)="open($event)">TG {{ id() }}</a>`,
})
export class TgLink {
  private readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  readonly id = input.required<string>();
  open(e: Event): void {
    e.preventDefault();
    e.stopPropagation();
    this.store.select('tg', this.id(), this.bp.drawer());
  }
}

/**
 * A network chip: colour swatch and name, opening its details. `hz`: a
 * frequency already shown next to it, left out of the name (the tooltip keeps it).
 */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-net',
  template: `<span class="netc" [title]="label()" (click)="open($event)"><span class="sw" [style.background]="color()"></span>{{ shortLabel() }}</span>`,
})
export class NetChip {
  private readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  readonly key = input.required<string>();
  readonly hz = input<number | null>(null);
  private readonly net = computed(() => this.store.ix()?.netByKey[this.key()]);
  readonly label = computed(() => this.net()?.label ?? this.key());
  readonly shortLabel = computed(() => {
    const hz = this.hz();
    return (hz ? this.label().replace(' · ' + mhz(hz) + ' MHz', '') : this.label()) || this.label();
  });
  readonly color = computed(() => this.store.colors.colorFor(this.store.activeFam() || '', this.net()));
  open(e: Event): void {
    e.stopPropagation();
    this.store.select('net', this.key(), this.bp.drawer());
  }
}

/** A colour swatch for a network. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-swatch',
  template: `<span class="sw" [style.background]="color()"></span>`,
})
export class Swatch {
  private readonly store = inject(NetStore);
  readonly key = input.required<string>();
  readonly color = computed(() => this.store.colors.colorFor(this.store.activeFam() || '', this.store.ix()?.netByKey[this.key()]));
}

/** Several network chips. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-nets',
  imports: [NetChip],
  template: `@for (k of keys(); track k; let i = $index) {@if (i) {{{ ' ' }}}<app-net [key]="k" />}`,
})
export class NetChips {
  readonly keys = input.required<string[]>();
}

export const CONFIDENCE_TIPS: Record<string, string> = {
  strong: 'Strong identity: a system id (P25 WACN/SysID, DMR network id, NXDN system code, TETRA MCC/MNC).',
  channel: 'Channel identity: a short code (or nothing) on a known frequency -- one conventional channel.',
  weak: 'Weak identity: only a short code on an unknown frequency; covers one stream.',
  none: 'Nothing identifying decoded yet.',
};

/** A network's identity level. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-conf',
  template: `<span class="badge" [class]="'b-' + c()" [title]="tip()" [attr.data-tip]="tip()">{{ c() === 'none' ? 'unidentified' : c() }}</span>`,
})
export class ConfidenceBadge {
  readonly c = input.required<Confidence>();
  readonly tip = computed(() => CONFIDENCE_TIPS[this.c()] || '');
}

/** A call's type badges: voice / data, group / private, emergency, encrypted, receivers. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-call-type',
  template: `<span>@if (c().data && !c().voice) {<span class="badge b-data">DATA</span>} @else {<span class="badge b-voice">VOICE</span>}@if (c().priv) {<span class="badge b-priv">PRIVATE</span>} @else {<span class="badge b-group">GROUP</span>}@if (c().emerg) {<span class="badge b-emerg">EMERGENCY</span>}@if (c().enc) {<span class="badge b-enc">ENCRYPTED</span>}@if (c().streams > 1) {<span class="badge b-group" [title]="rx()" [attr.data-tip]="rx()">{{ c().streams }} RX</span>}</span>`,
})
export class CallType {
  readonly c = input.required<NetCall>();
  readonly rx = computed(() => 'Heard by ' + this.c().streams + ' receivers (one call, deduplicated)');
}

/** A call's duration, running (with a live dot) while it is live. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-call-dur',
  template: `@if (live()) {<span class="livecell"><span class="dot"></span>{{ text() }}</span>} @else {{{ text() }}}`,
})
export class CallDuration {
  private readonly store = inject(NetStore);
  readonly c = input.required<NetCall>();
  readonly live = computed(() => isLive(this.c(), this.store.now(), this.store.fileMode()));
  readonly text = computed(() => dur(this.live() ? this.store.now() - this.c().start : this.c().last - this.c().start));
}

/** "→ TG 9" or "⇄ 1234" (private). */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-call-to',
  imports: [RadioLink, TgLink],
  template: `@if (!c().tgt) {—} @else if (c().priv) {<span>⇄ <app-radio [id]="c().tgt" /></span>} @else {<app-tg [id]="c().tgt" />}`,
})
export class CallTo {
  readonly c = input.required<NetCall>();
}

/** ▶ (play / stop, with the length) and ⤓ (download) for a call with audio. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-call-audio',
  template: `@if (c().audio && !store.fileMode()) {<span class="nowrap"><button class="play" type="button" [class.on]="on()" [attr.data-audio]="c().audio"
      [title]="title()" (click)="play($event)"><span>{{ on() ? '■' : '▶' }}</span>{{ c().audio_ms ? length() : '' }}</button>@if (download()) {<a class="dl" [href]="url()" [attr.download]="file()" title="Download this call's audio (.wav)" (click)="$event.stopPropagation()">⤓</a>}</span>}`,
})
export class CallAudio {
  protected readonly store = inject(NetStore);
  private readonly player = inject(Player);
  private readonly asr = inject(AsrService);
  readonly c = input.required<NetCall>();
  readonly download = input(true);
  readonly on = computed(() => this.player.playing() === this.c().audio);
  readonly length = computed(() => mmss(this.c().audio_ms || 0));
  readonly url = computed(() => NetApi.audioUrl(this.c().audio || ''));
  readonly file = computed(() => callFile(this.c()));
  readonly title = computed(() => "Play this call's audio" + (this.c().audio_ms ? ' (' + this.length() + ')' : '') +
    (this.asr.on() ? ' and transcribe it' : ''));
  play(e: Event): void {
    e.stopPropagation();
    this.player.toggle(this.c(), isLive(this.c(), this.store.now(), this.store.fileMode()));
  }
}

/** A call's transcript, if it has one. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-stt',
  template: `@if (t()?.t) {<span class="stt" [class.tx]="block()" [title]="'Speech-to-text (' + model() + '); may be wrong'">{{ t()!.t }}</span>}`,
  styles: [':host { display: contents; }'],
})
export class Transcript {
  private readonly asr = inject(AsrService);
  readonly c = input.required<NetCall>();
  readonly block = input(false);
  readonly t = computed(() => this.asr.transcript(this.c().audio));
  readonly model = computed(() => (this.t()?.m || '').replace(/^.*\//, ''));
}
