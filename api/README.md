# dsd-server API descriptions

Machine-readable descriptions of the dsd-server WebSocket JSON interface, plus
a self-contained interactive viewer. The single source of truth for the wire
format is [`../PROTOCOL.md`](../PROTOCOL.md) (generated from `session.cpp`);
these files are kept in sync with it.

| file | what it is |
|---|---|
| `openapi.yaml` | **OpenAPI 3.1**. Documents the WebSocket-upgrade endpoint and every JSON frame as reusable JSON Schemas (`components/schemas`) with `type` discriminators, enums, defaults, and real examples. Point Swagger UI / Redoc / RapiDoc at it. |
| `asyncapi.yaml` | **AsyncAPI 2.6**. Models the interface the way it behaves — one channel, messages both directions (`publish` = client→server, `subscribe` = server→client), including the binary IQ and audio frames. The more faithful spec for a bidirectional WebSocket. |
| `index.html` | **Interactive viewer**. A self-contained page (no build step) that renders `openapi.yaml` with RapiDoc and shows the AsyncAPI overview + full spec. Both specs are embedded, so it works offline. |

## View it interactively

- **OpenAPI** — open `index.html` in a browser (double-click, or serve the
  folder). It renders the OpenAPI spec with expandable schemas and examples,
  in light or dark theme. Nothing to install; RapiDoc loads from a CDN.
- **AsyncAPI** — the viewer's second tab shows the message flow and the full
  spec with a copy button. For the full interactive AsyncAPI treatment, paste
  the spec into [studio.asyncapi.com](https://studio.asyncapi.com).

## Why both

WebSocket is a bidirectional, message-driven protocol, which **AsyncAPI**
models natively (channels + messages). **OpenAPI** is request/response-shaped
and can only bolt the frames on as schemas under a placeholder upgrade
endpoint — but its tooling (Swagger UI / Redoc / RapiDoc, codegen, schema
validators) is ubiquitous, so it is the format many teams expect. The two
describe the same JSON; pick whichever your tooling speaks.

These are **client-side** artifacts. The server hand-writes and hand-parses
its JSON (see also `../proto/dsd_server.proto` for a protobuf mirror of the
same frames), so nothing here is a build or runtime dependency of the server.
