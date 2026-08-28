# dsd-server API description

A machine-readable description of the dsd-server WebSocket JSON interface, plus
a self-contained interactive viewer. The single source of truth for the wire
format is [`../PROTOCOL.md`](../PROTOCOL.md) (generated from `session.cpp`);
these files are kept in sync with it.

| file | what it is |
|---|---|
| `asyncapi.yaml` | **AsyncAPI 2.6**. Models the interface the way it behaves — one channel, messages both directions (`publish` = client→server, `subscribe` = server→client), including the binary IQ and audio frames, with `type`-discriminated message schemas, enums, defaults, and real examples. |
| `index.html` | **Interactive viewer**. A self-contained page (no build step, no runtime dependency) that renders `asyncapi.yaml` — expandable message cards with schema tables and examples, in light or dark theme. |

AsyncAPI is used rather than OpenAPI because this is a bidirectional,
message-driven WebSocket protocol, not request/response — AsyncAPI models
channels + messages natively, which OpenAPI cannot.

## View it interactively

- Open `index.html` in a browser (double-click, or serve the folder). It
  renders the spec directly — the spec is embedded, so it works offline and
  needs nothing installed.
- For the full AsyncAPI toolchain (generators, validators, an alternate
  renderer), paste `asyncapi.yaml` into
  [studio.asyncapi.com](https://studio.asyncapi.com).

This is a **client-side** artifact. The server hand-writes and hand-parses its
JSON (see also [`../proto/dsd_server.proto`](../proto/dsd_server.proto) for a
protobuf mirror of the same frames), so nothing here is a build or runtime
dependency of the server.
