# dsd-server web UI (Angular)

The browser UI of dsd-server -- the **status page** and the **network
explorer** -- as an Angular 22 application. It replaces the two pages built
into the server (`src/status_page.hpp`, `src/net_page.hpp`), with the same
looks and features, and talks to the server's existing JSON endpoints
(`/status.json`, `/log.json`, `/net.json`, `/net/...`; see `../PROTOCOL.md`).

The user manual for the explorer is `../docs/NET_EXPLORER.md`.

## Use it with dsd-server

```bash
cd webui
npm ci                 # once; needs Node >= 22.22.3 (or 24 LTS)
npm run build          # -> dist/webui/browser
DSD_WEBUI_DIR=$PWD/dist/webui/browser ../build/dsd-server 0.0.0.0 22600 4
```

With `DSD_WEBUI_DIR` set (to a folder holding the build's `index.html`),
dsd-server serves the app under **`/ui/`**: `/` and `/net` redirect to
`/ui/` (status) and `/ui/net` (explorer), and the built-in pages stay
available at `/classic` and `/classic/net`. Without it nothing changes --
the server keeps serving its built-in pages, so it never depends on Node.
The Docker image builds the app and sets `DSD_WEBUI_DIR` for you.

The app is static files: hashed bundles are cached by the browser for good,
`index.html` is revalidated, so a new build is picked up on reload. The
explorer's pages are served cross-origin isolated (as before) so
speech-to-text can use several threads on HTTPS / localhost.

## Develop

```bash
npm start              # ng serve on http://localhost:4200/ui/, proxying the
                       # server's endpoints to DSD_SERVER (default
                       # http://localhost:22600) -- see proxy.conf.mjs
npm test               # unit tests (Vitest, jsdom), once: npx ng test --watch=false
npm run build
```

## Layout

```
src/app/
  core/            models of the server's JSON, formatting helpers, HTTP services
  status/          the status page
  net/
    logic/         the explorer's pure logic: indexing, filters and sorting,
                   communities and detail lists, call rate, Pause list,
                   transcript cleaning, zip / CSV, export checks, graph model
                   and force layout, sharing unchanged data between polls
    state/         NetStore: the explorer's state as signals
    services/      audio player, speech-to-text (Web Worker), breakpoints, toast
    components/    the views (calls, talkgroups, radios, graph, links,
                   networks), details panel, now-playing bar, the generic
                   sortable table and small entity widgets
    net-page.*     the explorer shell: header actions, file / import bars,
                   protocol tabs, cards, network chips, polling
```

Standalone components, signals, zoneless change detection, OnPush
throughout. The stylesheet (`src/styles.css`) is the built-in explorer's,
unchanged, so both UIs look the same.

**Performance.** Every poll (1.5 s) brings the whole model; `logic/share.ts`
keeps the previous poll's objects for everything unchanged, so only new or
changed calls re-render (5000 calls: ~80 ms of main-thread work per poll in
Chrome, against ~130 ms for the built-in page).

**Tests** cover the logic (exhaustively), the services (audio player and
speech-to-text with a fake worker / audio element), the generic table, and
the two pages end to end against a mocked server (`HttpTestingController`).
`*.spec-helper.ts` files hold shared fixtures.
