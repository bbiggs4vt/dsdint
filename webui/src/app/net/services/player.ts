import { Injectable, InjectionToken, inject, signal } from '@angular/core';
import { NetCall } from '../../core/models';
import { NetApi } from '../../core/net-api.service';
import { AsrService } from './asr';
import { Toast } from './toast';

export const AUDIO_ELEMENT = new InjectionToken<() => HTMLAudioElement>('AUDIO_ELEMENT', {
  factory: () => () => new Audio(),
});

/** Plays calls' recorded audio; the call playing (or last played) shows in the now-playing bar. */
@Injectable()
export class Player {
  private readonly makeAudio = inject(AUDIO_ELEMENT);
  private readonly asr = inject(AsrService);
  private readonly toast = inject(Toast);
  private audio: HTMLAudioElement | null = null;

  /** The audio file playing now (null when stopped). */
  readonly playing = signal<string | null>(null);
  /** The call in the bar. */
  readonly call = signal<NetCall | null>(null);
  readonly time = signal(0);
  readonly duration = signal(0);

  private element(): HTMLAudioElement {
    if (!this.audio) {
      const a = (this.audio = this.makeAudio());
      a.addEventListener('ended', () => { this.playing.set(null); this.time.set(this.duration()); });
      a.addEventListener('error', () => {
        if (this.playing()) this.toast.show("This call's audio is no longer available.");
        this.playing.set(null);
      });
      a.addEventListener('timeupdate', () => {
        this.time.set(a.currentTime || 0);
        if (isFinite(a.duration) && a.duration > 0) this.duration.set(a.duration);
      });
    }
    return this.audio;
  }

  /** Play call `c` (again: stop it). With "Transcribe on play", also transcribe it. */
  toggle(c: NetCall, live: boolean): void {
    if (!c.audio) return;
    const a = this.element();
    if (this.playing() === c.audio) {
      a.pause();
      this.playing.set(null);
      return;
    }
    this.playing.set(c.audio);
    this.call.set(c);
    this.time.set(0);
    this.duration.set((c.audio_ms || 0) / 1000);
    a.src = NetApi.audioUrl(c.audio);
    try {
      const p = a.play();
      if (p && p.catch) p.catch(() => undefined);
    } catch { /* not supported here */ }
    if (this.asr.on()) this.asr.transcribe(c, live);
  }

  close(): void {
    if (this.audio && this.playing()) this.audio.pause();
    this.playing.set(null);
    this.call.set(null);
  }
}
