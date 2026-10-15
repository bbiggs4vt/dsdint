# Web UI architecture — decision notes (parked)

Status: **exploration / not decided.** Captured so we can come back to it.
Context: there is a branch that converts the embedded web UI to an Angular SPA.
This note records the trade-offs we talked through so the decision is grounded
when we pick it up.

## Today's architecture (what we're comparing against)

- The Network Explorer UI is **one hand-written vanilla-JS page** (HTML + CSS +
  JS) held as a raw string literal in `src/net_page.hpp`, compiled into the
  server and served at `/net`. A tiny `h()` helper builds the DOM; there is no
  framework.
- The **live data is not in the header** — the header holds the *UI code*. The
  radio data arrives at runtime over HTTP: the page polls `/net.json` (~1.5 s)
  and rebuilds the active view.
- Perf-sensitive pieces are hand-tuned: the force-directed **graph** (custom
  layout + SVG, `requestAnimationFrame`), the in-browser **spectrogram** (Web
  Audio + radix-2 FFT + canvas), and the big **call/radio tables** (row cap,
  renders only the visible view, holds updates while the user interacts, graph
  re-heats only on structural change).

## Untangle two decisions that keep getting merged

**1. Where the UI code lives** — embedded in the binary vs. a built bundle
served as static files (or also embedded).
**2. How the UI is built** — vanilla + manual DOM vs. an Angular SPA
(components, TypeScript, change detection, DI).

**(1) is ~orthogonal to runtime performance.** A built Angular bundle can be
gzipped and `.incbin`'d into the binary too. Embedding buys *deployment*
properties, not speed:

- single self-contained artifact,
- UI version always matches the server (we already rely on this for the
  built-in manual PDF),
- air-gapped / offline operation.

Costs of embedding (either framework): rebuild the binary to change the UI, and
a bigger binary. For load speed, **gzip + caching matter far more than
header-vs-file.**

So the real question is **(2) vanilla vs Angular**, and the real performance
story is the runtime, not the packaging.

## Where the current (vanilla, embedded) approach genuinely wins

- **No framework runtime.** Angular baseline is ~100–150 KB gzipped for
  hello-world, realistically 200–400 KB+ for an app like this; the current JS
  is tens of KB inline. On a LAN the transfer delta is ms, but
  **parse/compile/execute + memory** are not — they matter on tablets/phones,
  low-power/edge boxes, and cold first load.
- **No change-detection tax.** The 1.5 s poll just rebuilds the visible view;
  it's already tuned. Angular *can* match this but only with discipline
  (`OnPush`, `trackBy`, CDK virtual scroll, `runOutsideAngular` for the rAF
  loop / FFT).
- **Hand-tuned hot paths.** Graph layout, FFT, big tables — full control; a
  naive framework port risks regressing them.

Net: the current architecture has a **low effort-floor for good performance**
on this specific workload (high-frequency poll + partial re-render + a 60fps
graph + an FFT).

## Where Angular wins (the honest driver for a rewrite)

Almost never raw speed — it's **maintainability and scale**:

- a single ~4,000-line file is hard for multiple contributors; Angular gives
  components, DI, routing, typed models (TS catches a class of the bugs we hit
  by hand), reusable UI, forms, i18n, a11y tooling, a real unit/component test
  story (vs. the all-Playwright e2e we lean on now);
- ecosystem (ng-bootstrap, CDK virtual scroll, charts);
- feature velocity once the structure exists.

Costs: a build toolchain + CI, larger footprint, and the CD/perf discipline
above. Modern Angular (v16+ **signals**, standalone components, **zoneless**)
narrows the overhead gap.

## Decision heuristic

- **Driver = maintainability / multiple devs / long roadmap** → Angular is
  justified. Budget the perf work explicitly and keep the self-contained
  deployment by embedding the gzipped build.
- **Driver = raw load/runtime perf, tiny footprint, air-gapped single-binary
  simplicity, stable feature set** → the current embedded vanilla approach is
  already near-optimal; the rewrite's perf upside is small-to-negative unless
  carefully tuned, and risks regressing the hand-tuned graph/FFT.

## Zoneless + signals vs. a "hybrid" (framework-agnostic hot cores)

Question we landed on: if we go Angular, is there a benefit to keeping the
perf-critical pieces framework-agnostic (hybrid) **over** making the whole app
zoneless + signals?

**Zoneless + signals removes the main reason hybrid existed:**

- No Zone.js → a `requestAnimationFrame` loop no longer schedules change
  detection; the whole reason for `runOutsideAngular` disappears.
- Signals are pull-based → the poll only re-renders components bound to the
  signals that changed; no `markForCheck`/OnPush bookkeeping to get wrong.

So if CD overhead was the only motivation for hybrid, a **uniform
zoneless+signals app is simpler and more cohesive** (one mental model, better
cross-boundary typing, fewer interop seams) — an argument *against* maintaining
two architectures.

**The nuance that survives:** the hottest loops are signal-free regardless. The
graph mutates hundreds of node x/y **every frame**; the spectrogram is pure FFT
+ a canvas blit. You would **not** model those as signals — thousands of
signal reads/writes per frame with dependency tracking is *slower* than a plain
array + direct canvas/SVG writes. So an imperative "island" exists no matter
what. The only real choice is whether that island is a **clean,
framework-agnostic module** (thin component wrapper) or an imperative blob
inside a component. Making it agnostic is cheap and still earns:

1. **Portability / longevity hedge** — Angular reactivity already churned
   (Zone → signals → zoneless across majors); a reactivity-free core is immune
   to the next shift and reusable outside Angular.
2. **No per-frame reactivity tax, by construction** — can't accidentally
   signal-ify a path that has no framework imports.

That's not really "hybrid architecture" — it's **good module boundaries inside
a zoneless+signals app.**

**Where all-in beats hybrid:**

- Big tables → the lever is **virtualization** (DOM node count), not signals or
  agnostic code. Prefer **CDK virtual scroll** (Angular-native) over a bespoke
  agnostic table, unless the current hand-tuned table is genuinely better.
- Fewer leak surfaces — every agnostic-module ↔ component seam is a place to
  forget teardown (detached rAF loops, orphaned listeners).

## Caveats before committing to fully zoneless

- **Angular version:** zoneless was experimental/developer-preview for a while,
  stable around ~v20. On an older version it may not be production-ready and
  you'd fall back to OnPush + `runOutsideAngular`, which revives the hybrid
  rationale. **Check what the branch is on.**
- **Library stack:** CDK and ng-bootstrap are fine zoneless; some older /
  async-heavy third-party libs quietly depend on Zone and misbehave without it.
  **Audit the deps.**
- **Signal misuse is a real footgun:** signals in a 60fps loop are an
  anti-pattern (reinforces keeping the hot cores imperative).

## Tentative recommendation

Go **zoneless + signals for the whole app** — don't treat "hybrid" as a
separate architecture. Carry over only this from the hybrid idea: keep the
**force layout and the FFT as imperative, reactivity-free modules** behind a
thin component wrapper (plain state + direct canvas/SVG writes, never signals
per data point), and use **CDK virtual scroll** for the large lists. That
captures essentially all of hybrid's real value (no per-frame overhead,
portability) without the two-architectures tax.

If maintainability isn't the pressing driver and the feature set is stable,
the status-quo embedded vanilla UI is a legitimate "do nothing" that's already
near-optimal for this workload.

## To turn this into a go / no-go (next steps when we pick it up)

1. **Check the Angular branch's version** — is zoneless available/stable there,
   or would we be on OnPush + `runOutsideAngular`?
2. **Audit the dependency tree** for Zone-dependent libraries.
3. **See how the graph + spectrogram are wired** on the branch (component vs.
   imperative module).
4. **Profile both, head to head**, so the call is data-backed:
   - cold-load time + JS heap on the **lowest-end target device**
     (tablet / phone / edge box), vanilla vs. the Angular branch;
   - steady-state per-poll scripting time with a **large dataset** (thousands
     of calls/radios) and the **graph tab open** — that's where Angular CD
     either behaves (zoneless/OnPush + virtual scroll) or melts.

---

*Parked design note. No code change implied. See the conversation that
produced it for the longer form.*
