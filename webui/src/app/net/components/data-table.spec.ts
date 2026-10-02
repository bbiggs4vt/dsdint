import { ChangeDetectionStrategy, Component, signal } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Breakpoints } from '../services/breakpoints';
import { NetStore } from '../state/net-store';
import { FakeBreakpoints, explorerProviders, tick } from '../testing.spec-helper';
import { CardTpl, Col, DataTable, ROWS, SubTpl } from './data-table';

interface Row { id: string; n: number; note: string; }

@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [DataTable, Col, CardTpl, SubTpl],
  providers: [NetStore],
  template: `<app-table [rows]="rows()" view="t" empty="Nothing." [trackBy]="id" [limit]="limit()" [clickable]="true"
                        [isSelected]="isSel" [hasSub]="hasNote" (rowClick)="clicked.set($event.id)">
    <ng-template appCol="Id" [sortKey]="byId" [dir]="1" let-r>{{ r.id }}</ng-template>
    <ng-template appCol="N" cls="num" [sortKey]="byN" let-r>{{ r.n }}</ng-template>
    <ng-template appCol="Note" [hideEmpty]="hasNote" let-r>{{ r.note }}</ng-template>
    <ng-template appSub let-r>sub:{{ r.note }}</ng-template>
    <ng-template appCard let-r><div class="card-row">{{ r.id }}/{{ r.n }}</div></ng-template>
  </app-table>`,
})
class Host {
  readonly rows = signal<Row[]>([{ id: 'TG 9', n: 2, note: '' }, { id: 'TG 10', n: 5, note: '' }, { id: 'TG 100', n: 1, note: '' }]);
  readonly limit = signal(ROWS);
  readonly clicked = signal('');
  readonly id = (r: Row) => r.id;
  readonly byId = (r: Row) => r.id;
  readonly byN = (r: Row) => r.n;
  readonly hasNote = (r: Row) => !!r.note;
  readonly isSel = (r: Row) => r.id === 'TG 10';
}

describe('DataTable', () => {
  let fixture: ComponentFixture<Host>;
  let el: HTMLElement;
  const ths = () => Array.from(el.querySelectorAll('th')).map((t) => t.textContent!.trim());
  const firstCol = () => Array.from(el.querySelectorAll('tbody tr:not(.sub) td:first-child')).map((t) => t.textContent!.trim());
  async function render() { await tick(); await fixture.whenStable(); }

  beforeEach(async () => {
    TestBed.configureTestingModule({ providers: explorerProviders() });
    fixture = TestBed.createComponent(Host);
    el = fixture.nativeElement;
    await render();
  });

  it('renders rows in the given order; hides an empty column', () => {
    expect(ths()).toEqual(['Id', 'N']);
    expect(firstCol()).toEqual(['TG 9', 'TG 10', 'TG 100']);
    expect(el.querySelector('tbody tr.sel')!.textContent).toContain('TG 10');
    expect(el.querySelector('th.num')!.textContent).toBe('N');
  });

  it('sorts by a column (natural order), reverses on a second click', async () => {
    (el.querySelectorAll('th')[0] as HTMLElement).click();
    await render();
    expect(firstCol()).toEqual(['TG 9', 'TG 10', 'TG 100']);
    expect(ths()[0]).toBe('Id ▲');
    (el.querySelectorAll('th')[0] as HTMLElement).click();
    await render();
    expect(firstCol()).toEqual(['TG 100', 'TG 10', 'TG 9']);
    (el.querySelectorAll('th')[1] as HTMLElement).click();     // numbers: largest first
    await render();
    expect(firstCol()).toEqual(['TG 10', 'TG 9', 'TG 100']);
    expect(ths()[1]).toBe('N ▼');
  });

  it('shows a column and sub lines once a row has them', async () => {
    fixture.componentInstance.rows.update((r) => [{ ...r[0], note: 'hello' }, ...r.slice(1)]);
    await render();
    expect(ths()).toEqual(['Id', 'N', 'Note']);
    const sub = el.querySelector('tbody tr.sub')!;
    expect(sub.textContent!.trim()).toBe('sub:hello');
    expect(sub.querySelector('td')!.getAttribute('colspan')).toBe('3');
    expect(el.querySelector('tbody tr.hassub')!.textContent).toContain('TG 9');
  });

  it('caps the rows and says how many there are', async () => {
    fixture.componentInstance.limit.set(2);
    await render();
    expect(firstCol().length).toBe(2);
    expect(el.querySelector('.more')!.textContent).toContain('Showing 2 of 3');
  });

  it('reports row clicks; says when it is empty', async () => {
    (el.querySelectorAll('tbody tr')[2] as HTMLElement).click();
    expect(fixture.componentInstance.clicked()).toBe('TG 100');
    fixture.componentInstance.rows.set([]);
    await render();
    expect(el.querySelector('td.empty')!.textContent).toBe('Nothing.');
  });

  it('on a phone: cards and a sort menu', async () => {
    (TestBed.inject(Breakpoints) as unknown as FakeBreakpoints).phone.set(true);
    await render();
    expect(el.querySelector('table')).toBeNull();
    expect(Array.from(el.querySelectorAll('.card-row')).map((c) => c.textContent)).toEqual(['TG 9/2', 'TG 10/5', 'TG 100/1']);
    const sel = el.querySelector('.sortbar select') as HTMLSelectElement;
    expect(Array.from(sel.options).map((o) => o.text)).toEqual(['Default order', 'Id ▲', 'Id ▼', 'N ▼', 'N ▲']);
    sel.value = sel.options[4].value;                      // N ascending
    sel.dispatchEvent(new Event('change'));
    await render();
    expect(Array.from(el.querySelectorAll('.card-row')).map((c) => c.textContent)).toEqual(['TG 100/1', 'TG 9/2', 'TG 10/5']);
    expect(el.querySelector('.sortbar')!.textContent).toContain('3 rows');
  });
});
