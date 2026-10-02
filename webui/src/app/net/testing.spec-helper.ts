import { provideHttpClient, withFetch } from '@angular/common/http';
import { HttpTestingController, provideHttpClientTesting } from '@angular/common/http/testing';
import { Injectable, signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';
import { provideRouter } from '@angular/router';
import { AsrConfig } from '../core/models';
import { ASR_WORKER, DECODE_16K } from './services/asr';
import { Breakpoints } from './services/breakpoints';
import { AUDIO_ELEMENT } from './services/player';

/** Breakpoints the tests set by hand. */
@Injectable()
export class FakeBreakpoints {
  readonly phone = signal(false);
  readonly drawer = signal(false);
  readonly mid = signal(false);
  readonly coarse = signal(false);
}

/** A Worker stand-in: records what it was sent; the test answers through `reply`. */
export class FakeWorker {
  static last: FakeWorker | null = null;
  sent: { cmd: string; [k: string]: unknown }[] = [];
  onmessage: ((e: MessageEvent) => void) | null = null;
  onerror: ((e: ErrorEvent) => void) | null = null;
  terminated = false;
  constructor() { FakeWorker.last = this; }
  postMessage(m: { cmd: string }): void { this.sent.push(m); }
  terminate(): void { this.terminated = true; }
  reply(data: unknown): void { this.onmessage?.({ data } as MessageEvent); }
}

/** An <audio> stand-in. */
export class FakeAudio extends EventTarget {
  src = '';
  currentTime = 0;
  duration = NaN;
  paused = true;
  play(): Promise<void> { this.paused = false; return Promise.resolve(); }
  pause(): void { this.paused = true; }
}

export const LOCAL_ASR: AsrConfig = { local: true, lib: true, models: ['Xenova/whisper-base', 'Xenova/whisper-small'],
                                      model: 'Xenova/whisper-small', language: 'english' };

export function explorerProviders() {
  localStorage.clear();
  FakeWorker.last = null;
  return [
    provideHttpClient(withFetch()), provideHttpClientTesting(), provideRouter([]),
    { provide: Breakpoints, useClass: FakeBreakpoints },
    { provide: ASR_WORKER, useValue: () => new FakeWorker() as unknown as Worker },
    { provide: DECODE_16K, useValue: () => Promise.resolve(new Float32Array(16000)) },
    { provide: AUDIO_ELEMENT, useValue: () => new FakeAudio() as unknown as HTMLAudioElement },
  ];
}

export function http(): HttpTestingController {
  return TestBed.inject(HttpTestingController);
}

/** Lets pending promises and timers (0 ms) run. */
export function tick(ms = 0): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}
