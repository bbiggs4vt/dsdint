import { Component, computed, inject, ChangeDetectionStrategy } from '@angular/core';
import { ago, keys, mhz } from '../../core/format';
import { NetNetwork, NetRadio, NetTalkgroup } from '../../core/models';
import { communities, communityNetworks, crossNetwork, hubRadios, ranked } from '../logic/links';
import { Breakpoints } from '../services/breakpoints';
import { NetStore } from '../state/net-store';
import { Col, DataTable } from './data-table';
import { ConfidenceBadge, NetChip, NetChips, RadioLink, TgLink } from './widgets';

/** Talkgroups: radios, calls, emergency / encrypted counts, networks, first / last heard. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-tgs-view',
  imports: [DataTable, Col, TgLink, NetChips],
  template: `<div class="scroll"><app-table [rows]="rows()" view="tgs" empty="No talkgroups yet." [trackBy]="id" [clickable]="true"
      [isSelected]="selected" (rowClick)="open($event)">
    <ng-template appCol="Talkgroup" [sortKey]="byId" [dir]="1" let-t><app-tg [id]="t.id" /></ng-template>
    <ng-template appCol="Networks" let-t><app-nets [keys]="t.networks" /></ng-template>
    <ng-template appCol="Radios" cls="num" [sortKey]="byRadios" let-t>{{ count(t.radios) }}</ng-template>
    <ng-template appCol="Calls" cls="num" [sortKey]="byCalls" let-t>{{ t.calls }}</ng-template>
    <ng-template appCol="Emerg." cls="num" [sortKey]="byEmerg" let-t>@if (t.emerg) {<span class="badge b-emerg">{{ t.emerg }}</span>} @else {—}</ng-template>
    <ng-template appCol="Encr." cls="num" [sortKey]="byEnc" let-t>@if (t.enc) {<span class="badge b-enc">{{ t.enc }}</span>} @else {—}</ng-template>
    <ng-template appCol="First heard" cls="nowrap" [sortKey]="byFirst" let-t>{{ ago(t.first, store.now()) }}</ng-template>
    <ng-template appCol="Last heard" cls="nowrap" [sortKey]="byLast" let-t>{{ ago(t.last, store.now()) }}</ng-template>
  </app-table></div>`,
})
export class TalkgroupsView {
  protected readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  protected readonly ago = ago;
  readonly rows = computed(() => this.store.tgs().slice().sort((a, b) => b.last - a.last));
  readonly id = (t: NetTalkgroup) => t.id;
  readonly byId = (t: NetTalkgroup) => t.id;
  readonly byRadios = (t: NetTalkgroup) => keys(t.radios).length;
  readonly byCalls = (t: NetTalkgroup) => t.calls;
  readonly byEmerg = (t: NetTalkgroup) => t.emerg;
  readonly byEnc = (t: NetTalkgroup) => t.enc;
  readonly byFirst = (t: NetTalkgroup) => t.first;
  readonly byLast = (t: NetTalkgroup) => t.last;
  readonly selected = (t: NetTalkgroup) => this.store.isSel('tg', t.id);
  count(o: Record<string, number>): number { return keys(o).length; }
  open(t: NetTalkgroup): void { this.store.select('tg', t.id, this.bp.drawer()); }
}

/** Radios: busiest talkgroups, counts, private peers, networks, last heard. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-radios-view',
  imports: [DataTable, Col, RadioLink, TgLink, NetChips],
  template: `<div class="scroll"><app-table [rows]="rows()" view="radios" empty="No radios yet." [trackBy]="id" [clickable]="true"
      [isSelected]="selected" (rowClick)="open($event)">
    <ng-template appCol="Radio" [sortKey]="byId" [dir]="1" let-r><app-radio [id]="r.id" /></ng-template>
    <ng-template appCol="Talkgroups" let-r>@let ts = topTgs(r);
      @for (t of ts.slice(0, 4); track t.id; let i = $index) {@if (i) {<span>, </span>}<app-tg [id]="t.id" />}
      @if (ts.length > 4) {<span class="alias"> +{{ ts.length - 4 }}</span>}
      @if (!ts.length) {—}</ng-template>
    <ng-template appCol="# TGs" cls="num" [sortKey]="byTgs" let-r>{{ count(r.tgs) }}</ng-template>
    <ng-template appCol="Private peers" cls="num" [sortKey]="byPeers" let-r>{{ count(r.peers) || '—' }}</ng-template>
    <ng-template appCol="Networks" let-r><app-nets [keys]="r.networks" /></ng-template>
    <ng-template appCol="Calls" cls="num" [sortKey]="byCalls" let-r>{{ r.calls }}</ng-template>
    <ng-template appCol="Last heard" cls="nowrap" [sortKey]="byLast" let-r>{{ ago(r.last, store.now()) }}</ng-template>
  </app-table></div>`,
})
export class RadiosView {
  protected readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  protected readonly ago = ago;
  readonly rows = computed(() => this.store.radios().slice().sort((a, b) => b.last - a.last));
  readonly id = (r: NetRadio) => r.id;
  readonly byId = (r: NetRadio) => r.id;
  readonly byTgs = (r: NetRadio) => keys(r.tgs).length;
  readonly byPeers = (r: NetRadio) => keys(r.peers).length;
  readonly byCalls = (r: NetRadio) => r.calls;
  readonly byLast = (r: NetRadio) => r.last;
  readonly selected = (r: NetRadio) => this.store.isSel('radio', r.id);
  topTgs(r: NetRadio) { return ranked(r.tgs); }
  count(o: Record<string, number>): number { return keys(o).length; }
  open(r: NetRadio): void { this.store.select('radio', r.id, this.bp.drawer()); }
}

/** Networks: identity, identifiers, sites, channels and counts. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-nets-view',
  imports: [DataTable, Col, NetChip, ConfidenceBadge],
  template: `<div class="scroll"><app-table [rows]="rows()" view="nets" empty="No networks identified yet." [trackBy]="key" [clickable]="true"
      [isSelected]="selected" (rowClick)="open($event)">
    <ng-template appCol="Network" [sortKey]="byLabel" [dir]="1" let-n><app-net [key]="n.key" /></ng-template>
    <ng-template appCol="Identity" let-n><app-conf [c]="n.confidence" /></ng-template>
    <ng-template appCol="Identifiers" cls="ids" [hideMd]="true" let-n>{{ ids(n) }}</ng-template>
    <ng-template appCol="Sites" let-n>{{ n.sites.join(', ') || '—' }}</ng-template>
    <ng-template appCol="Channels (MHz)" cls="mono" [dropEmpty]="hasFreqs" let-n>{{ freqs(n) || '—' }}</ng-template>
    <ng-template appCol="TGs" cls="num" [sortKey]="byTgs" let-n>{{ ix()?.netTg?.[n.key] || 0 }}</ng-template>
    <ng-template appCol="Radios" cls="num" [sortKey]="byRadios" let-n>{{ ix()?.netRad?.[n.key] || 0 }}</ng-template>
    <ng-template appCol="Calls" cls="num" [sortKey]="byCalls" let-n>{{ n.calls }}</ng-template>
    <ng-template appCol="Streams" cls="num" [hideMd]="true" [sortKey]="bySessions" let-n>{{ n.sessions }}</ng-template>
    <ng-template appCol="Last heard" cls="nowrap" [sortKey]="byLast" let-n>{{ ago(n.last, store.now()) }}</ng-template>
  </app-table></div>`,
})
export class NetworksView {
  protected readonly store = inject(NetStore);
  private readonly bp = inject(Breakpoints);
  protected readonly ago = ago;
  readonly ix = this.store.ix;
  readonly rows = computed(() => (this.ix()?.nets || []).slice().sort((a, b) => b.last - a.last));
  readonly key = (n: NetNetwork) => n.key;
  readonly byLabel = (n: NetNetwork) => n.label;
  readonly byTgs = (n: NetNetwork) => this.ix()?.netTg[n.key] || 0;
  readonly byRadios = (n: NetNetwork) => this.ix()?.netRad[n.key] || 0;
  readonly byCalls = (n: NetNetwork) => n.calls;
  readonly bySessions = (n: NetNetwork) => n.sessions;
  readonly byLast = (n: NetNetwork) => n.last;
  readonly hasFreqs = (n: NetNetwork) => !!n.freqs?.length;
  readonly selected = (n: NetNetwork) => this.store.isSel('net', n.key);
  ids(n: NetNetwork): string { return keys(n.ids).map((k) => k + '=' + n.ids[k]).join('  '); }
  freqs(n: NetNetwork): string { return (n.freqs || []).map(mhz).join(', '); }
  open(n: NetNetwork): void { this.store.select('net', n.key, this.bp.drawer()); }
}

/** Links: talk communities, talkgroups / radios on more than one network, hub radios. */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-links-view',
  imports: [RadioLink, TgLink, NetChips],
  templateUrl: './links-view.html',
})
export class LinksView {
  private readonly store = inject(NetStore);
  protected readonly keys = keys;
  readonly comms = computed(() => {
    const ix = this.store.ix();
    if (!ix) return [];
    return communities(this.store.tgs(), this.store.radios()).slice(0, 60).map((c, i) => ({
      n: i + 1, radios: c.r.length, tgCount: c.t.length, nets: communityNetworks(c, ix),
      tgs: c.t.slice().sort((a, b) => (ix.tgById[b]?.calls || 0) - (ix.tgById[a]?.calls || 0)),
      rads: c.r.slice().sort((a, b) => (ix.rById[b]?.calls || 0) - (ix.rById[a]?.calls || 0)),
    }));
  });
  readonly total = computed(() => communities(this.store.tgs(), this.store.radios()).length);
  readonly cross = computed(() => crossNetwork(this.store.tgs(), this.store.radios()));
  readonly hubs = computed(() => hubRadios(this.store.radios()).slice(0, 40).map((r) => ({ r, tgs: ranked(r.tgs) })));
  readonly hubCount = computed(() => hubRadios(this.store.radios()).length);
}
