import { Injectable, signal } from '@angular/core';

/** A short message at the bottom of the window (3.5 s). */
@Injectable()
export class Toast {
  readonly text = signal<string | null>(null);
  private t: ReturnType<typeof setTimeout> | undefined;
  show(text: string, ms = 3500): void {
    this.text.set(text);
    clearTimeout(this.t);
    this.t = setTimeout(() => this.text.set(null), ms);
  }
}
