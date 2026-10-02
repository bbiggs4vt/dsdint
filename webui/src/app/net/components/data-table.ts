import { NgTemplateOutlet } from '@angular/common';
import { Component, Directive, TemplateRef, computed, contentChild, contentChildren, inject, input, output, ChangeDetectionStrategy } from '@angular/core';
import { SortKey, sortRows } from '../logic/filters';
import { Breakpoints } from '../services/breakpoints';
import { NetStore } from '../state/net-store';

/** Rows shown at a time. */
export const ROWS = 400;

/** A column: `<ng-template appCol="MHz" cls="mono" [sortKey]="byFreq" let-row>…</ng-template>`. */
@Directive({ selector: 'ng-template[appCol]' })
export class Col {
  readonly tpl = inject<TemplateRef<{ $implicit: any }>>(TemplateRef);   // eslint-disable-line @typescript-eslint/no-explicit-any
  readonly label = input.required<string>({ alias: 'appCol' });
  readonly cls = input('');
  readonly sortKey = input<SortKey<any> | null>(null);                    // eslint-disable-line @typescript-eslint/no-explicit-any
  /** First click sorts this way (-1 descending, 1 ascending). */
  readonly dir = input(-1);
  /** Shown only if some row has a value. */
  readonly hideEmpty = input<((r: any) => boolean) | null>(null);         // eslint-disable-line @typescript-eslint/no-explicit-any
  /** On narrow screens, dropped unless some row has a value. */
  readonly dropEmpty = input<((r: any) => boolean) | null>(null);         // eslint-disable-line @typescript-eslint/no-explicit-any
  /** Dropped on mid-size screens (the details show it). */
  readonly hideMd = input(false);
}
/** How a row looks on a phone (cards instead of the table). */
@Directive({ selector: 'ng-template[appCard]' })
export class CardTpl {
  readonly tpl = inject<TemplateRef<{ $implicit: any }>>(TemplateRef);   // eslint-disable-line @typescript-eslint/no-explicit-any
}
/** A full-width line under a row (a call's transcript). */
@Directive({ selector: 'ng-template[appSub]' })
export class SubTpl {
  readonly tpl = inject<TemplateRef<{ $implicit: any }>>(TemplateRef);   // eslint-disable-line @typescript-eslint/no-explicit-any
}

/**
 * A sortable table (sort state kept per view in the store), showing up to
 * ROWS rows. On phones it renders cards (if given) or stacked rows, with a
 * sort menu in place of the column headers.
 */
@Component({
  changeDetection: ChangeDetectionStrategy.OnPush,
  selector: 'app-table',
  imports: [NgTemplateOutlet],
  templateUrl: './data-table.html',
  styles: [':host { display: block; }'],
})
export class DataTable<T> {
  private readonly store = inject(NetStore);
  protected readonly bp = inject(Breakpoints);
  readonly rows = input.required<T[]>();
  readonly view = input.required<string>();
  readonly empty = input('');
  readonly trackBy = input.required<(r: T) => string | number>();
  readonly isSelected = input<((r: T) => boolean) | null>(null);
  /** Rows react to clicks (and look clickable). */
  readonly clickable = input(false);
  /** Which rows have a sub line. */
  readonly hasSub = input<((r: T) => boolean) | null>(null);
  /** Rows shown at most. */
  readonly limit = input(ROWS);
  readonly rowClick = output<T>();

  readonly cols = contentChildren(Col);
  readonly card = contentChild(CardTpl);
  readonly sub = contentChild(SubTpl);

  readonly shown = computed(() => {
    const rows = this.rows(), drawer = this.bp.drawer(), mid = this.bp.mid();
    return this.cols().filter((c) => {
      const he = c.hideEmpty(), de = c.dropEmpty();
      if (he && !rows.some(he)) return false;
      if (drawer && c.hideMd() && mid) return false;
      if (drawer && de && !rows.some(de)) return false;
      return true;
    });
  });
  readonly sortState = computed(() => this.store.sort()[this.view()] || null);
  /** The rows in display order (all of them; ROWS are shown). */
  readonly sorted = computed(() => {
    const st = this.sortState(), col = st ? this.cols()[st.i] : undefined, key = col?.sortKey();
    return key ? sortRows(this.rows(), key, st!.dir) : this.rows();
  });
  readonly visible = computed(() => this.sorted().slice(0, this.limit()));
  readonly phoneCards = computed(() => !!this.card() && this.bp.phone());

  index(c: Col): number {
    return this.cols().indexOf(c);
  }
  arrow(c: Col): string {
    const st = this.sortState();
    return st && st.i === this.index(c) ? (st.dir > 0 ? ' ▲' : ' ▼') : '';
  }
  /** Header click: sort by this column, or reverse it. */
  sortBy(c: Col): void {
    if (!c.sortKey()) return;
    const i = this.index(c), cur = this.sortState();
    this.setSort(cur && cur.i === i ? { i, dir: -cur.dir } : { i, dir: c.dir() });
  }
  /** The phone's sort menu ("" = default order). */
  sortMenu(value: string): void {
    if (!value) { this.setSort(null); return; }
    const [i, dir] = value.split(':').map(Number);
    this.setSort({ i, dir });
  }
  private setSort(s: { i: number; dir: number } | null): void {
    this.store.sort.update((m) => ({ ...m, [this.view()]: s }));
  }
  key(r: T): string | number {
    return this.trackBy()(r);
  }
  selected(r: T): boolean {
    const f = this.isSelected();
    return !!f && f(r);
  }
  subFor(r: T): boolean {
    const f = this.hasSub();
    return !!this.sub() && !!f && f(r);
  }
  click(r: T): void {
    if (this.clickable()) this.rowClick.emit(r);
  }
  isNum(c: Col): boolean {
    return /\bnum\b/.test(c.cls());
  }
}
