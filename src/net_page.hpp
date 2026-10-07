// net_page.hpp
//
// The network explorer page (GET /net): a live, self-contained (no CDN) view
// of the AssocModel (assoc_model.hpp) -- calls, talkgroups, radios, networks,
// a force-directed association graph, and a "links" analysis of radios and
// talkgroups tied together through shared talkgroups / private calls or seen
// on more than one network. Everything is per protocol family.
//
// The page is static; its script polls /net.json and renders client-side.
// Speech-to-text (a call's audio transcribed when it is played) runs in the
// browser; its library and model are loaded only then, from the server's
// /net/asr/ folder -- or from the internet only if the user picks that.
// Every decoder-derived string (ids, aliases, SMS text, network labels) is
// inserted with textContent / setAttribute, never as HTML.

#pragma once

#include <string>

namespace dsdsrv {

inline std::string render_net_page_html() {
    return R"NETPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>dsd-server network explorer</title>
<link rel="icon" href="data:,">
<style>
  :root {
    --bg:#272b30; --panel:#3a3f44; --panel2:#32373c; --text:#c8c8c8; --heading:#fff;
    --muted:#7a8288; --success:#62c462; --info:#5bc0de; --warn:#f89406; --danger:#ee5f5b;
    --purple:#b38bff; --comp-bd:rgba(0,0,0,.6); --table-bd:#1c1e22;
    color-scheme: dark;
  }
  * { box-sizing: border-box; }
  html, body { background: var(--bg); }
  body { color: var(--text); margin: 0; padding: 0 0 2rem;
         font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif; font-size: 14px; line-height: 1.42857; }
  a { color: var(--info); text-decoration: none; }
  a:hover { text-decoration: underline; }
  .wrap { max-width: 1500px; margin: 0 auto; padding: 0 1.25rem; }
  header { background-image: linear-gradient(#484e55, #3a3f44 60%, #2e3236);
           border-bottom: 1px solid var(--comp-bd); box-shadow: inset 0 1px 0 rgba(255,255,255,.08);
           margin-bottom: 1.1rem; }
  .hdr { display: flex; align-items: center; justify-content: space-between; gap: 1rem;
         flex-wrap: wrap; padding-top: .8rem; padding-bottom: .8rem; }
  h1 { font-size: 1.4rem; margin: 0; font-weight: 500; color: var(--heading); text-shadow: 0 -1px 0 rgba(0,0,0,.4); }
  h1 .accent { color: var(--info); }
  .sub { color: var(--muted); font-size: .82rem; margin-top: .2rem; }
  /* The status text has a fixed width: a longer status must not re-wrap the
     header and move the page (a click in progress would land on another row). */
  #live { display: inline-block; width: 11em; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
          vertical-align: bottom; }
  .hdr-actions { display: flex; gap: .5rem; align-items: center; flex-wrap: wrap; }
  /* The title takes the room the actions leave (its status line wraps)
     rather than pushing the actions onto a row of their own. */
  .ttl { flex: 1 1 18rem; min-width: 0; }
  input[type=search] { background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd);
                       border-radius: 4px; padding: .35rem .6rem; font: inherit; width: 16rem; max-width: 100%; }
  .btn { appearance: none; cursor: pointer; color: var(--heading);
         background-image: linear-gradient(rgba(255,255,255,.12), rgba(255,255,255,0)), linear-gradient(#7a8288, #7a8288);
         border: 1px solid var(--comp-bd); border-radius: 4px; padding: .3rem .8rem; font: inherit; font-size: .8rem;
         text-shadow: 0 -1px 0 rgba(0,0,0,.3); }
  a.btn { display: inline-block; text-decoration: none; }
  #help { font-weight: 700; }
  #help .hl { display: none; font-weight: 400; }
  .btn:hover { background-image: linear-gradient(rgba(255,255,255,.18), rgba(255,255,255,.03)), linear-gradient(#7a8288, #7a8288); }
  .btn.recon { background-image: linear-gradient(rgba(255,255,255,.12), rgba(255,255,255,0)), linear-gradient(#d9534f, #c9302c); }
  .dropdown { position: relative; display: inline-block; }
  .menu { position: absolute; right: 0; top: calc(100% + 4px); z-index: 20; min-width: 17rem;
          background: var(--panel); border: 1px solid var(--comp-bd); border-radius: 4px;
          box-shadow: 0 6px 18px rgba(0,0,0,.45); padding: .25rem 0; }
  .menu a { display: block; padding: .45rem .8rem; color: var(--heading); text-decoration: none; }
  .menu a:hover { background: rgba(255,255,255,.07); }
  .menu a small { display: block; color: var(--muted); font-size: .72rem; }
  .filebar { background: #2c4a56; border: 1px solid var(--info); color: var(--heading); border-radius: 4px;
             padding: .5rem .8rem; margin-bottom: .9rem; display: flex; flex-wrap: wrap; gap: .4rem .8rem;
             align-items: center; }
  .filebar .m { color: #b9d7e1; font-size: .82rem; }
  .filebar.imp { background: #45402f; border-color: #c9a243; }
  .filebar.imp .m { color: #e3d3a6; }
  .filebar .imp-item { background: rgba(0,0,0,.25); border-radius: 3px; padding: .1rem .45rem; font-size: .8rem; }
  .filebar .imp-item a { margin-left: .35rem; }
  .filebar .x { cursor: pointer; color: var(--muted); margin-left: auto; }
  .report { width: 100%; margin: .2rem 0 0; padding: 0; list-style: none; font-size: .8rem; }
  .report li { padding: .1rem 0; }
  .report .st { display: inline-block; min-width: 4.8rem; font-weight: 600; text-transform: uppercase; font-size: .7rem; letter-spacing: .04em; }
  .report .st.ok { color: var(--success); } .report .st.skip { color: var(--muted); }
  .report .st.bad { color: var(--danger); }
  body.filemode .live-only { display: none !important; }
  /* Developer tools (Record): shown with /net?dev=1 (this browser) or a
     server run with DSD_NET_DEV=1. */
  body:not(.devtools) .dev-only { display: none !important; }
  body.dragging { outline: 3px dashed var(--info); outline-offset: -6px; }
  .recst { color: var(--muted); font-size: .78rem; max-width: 26rem; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .recst .live { color: var(--danger); font-weight: 600; }
  .btn.on { background-image: linear-gradient(rgba(255,255,255,.12), rgba(255,255,255,0)), linear-gradient(#e0a33c, #d38f2a); }
  .tabs { display: flex; gap: .3rem; margin-bottom: .7rem; border-bottom: 1px solid var(--table-bd); flex-wrap: wrap; }
  .tab { appearance: none; cursor: pointer; color: var(--muted); background: transparent;
         border: 1px solid transparent; border-bottom: none; border-radius: 4px 4px 0 0; padding: .4rem .9rem;
         font: inherit; font-size: .8rem; text-transform: uppercase; letter-spacing: .05em; margin-bottom: -1px; }
  .tab:hover { color: var(--heading); }
  .tab.active { color: var(--heading); border-color: var(--table-bd); background-image: linear-gradient(#41474d, #3a3f44);
                border-bottom: 1px solid var(--panel); }
  .tab .count { color: var(--muted); font-weight: 400; }
  .tab.active .count { color: var(--info); }
  .famtabs .tab { font-size: .9rem; letter-spacing: .03em; }
  /* Connections: a dot on a protocol tab with a stream connected (green:
     decoding now; hollow: connected, quiet), and the summary at the row's end. */
  .cdot { display: inline-block; width: .55em; height: .55em; border-radius: 50%; margin-right: .4em; vertical-align: .08em;
          border: 1.5px solid var(--muted); }
  .cdot.on { background: var(--success, #62c462); border-color: var(--success, #62c462); box-shadow: 0 0 5px rgba(98,196,98,.6); }
  .famtabs .conn { margin-left: auto; align-self: center; color: var(--muted); font-size: .8rem; padding: 0 .3rem; white-space: nowrap; }
  .famtabs .conn.on { color: var(--text); }
  .famtabs .imps { margin-left: auto; align-self: center; font-size: .8rem; padding: 0 .3rem; white-space: nowrap; color: #e3c66f; }
  .famtabs .imps + .conn { margin-left: .9rem; }
  .cards { display: flex; flex-wrap: wrap; gap: .8rem; margin-bottom: 1rem; }
  .card { background-image: linear-gradient(#3e444a, #3a3f44 60%, #363b40); border: 1px solid var(--comp-bd);
          border-radius: 4px; box-shadow: inset 0 1px 0 rgba(255,255,255,.06); padding: .65rem 1rem; min-width: 8.5rem; }
  .card .n { font-size: 1.7rem; font-weight: 500; color: var(--heading); font-variant-numeric: tabular-nums; }
  .card .l { color: var(--muted); font-size: .7rem; text-transform: uppercase; letter-spacing: .06em; }
  .card.live .n { color: var(--success); }
  .filters { margin-bottom: 1rem; }
  .frow { display: flex; flex-wrap: wrap; gap: .4rem; align-items: center; }
  .filters.open .frow + .frow { margin-top: .45rem; }
  .frow .lbl { color: var(--muted); font-size: .72rem; text-transform: uppercase; letter-spacing: .06em; min-width: 6.5rem; }
  .ftog { cursor: pointer; user-select: none; background: none; border: 0; padding: .15rem .2rem; font: inherit; display: inline-flex;
          align-items: center; gap: .3rem; color: var(--muted); font-size: .72rem; text-transform: uppercase; letter-spacing: .06em;
          margin-right: .2rem; }
  .filters.open .ftog { min-width: 6.5rem; }
  .ftog:hover { color: var(--text); }
  .ftog .car { display: inline-block; width: .8rem; transition: transform .15s; }
  .filters.open .ftog .car { transform: rotate(90deg); }
  .fcount { background: var(--info); color: #12171b; border-radius: 8px; padding: 0 .4rem; font-size: .68rem; font-weight: 600; letter-spacing: 0; }
  .fnone, .fhint { color: var(--muted); font-size: .78rem; }
  .fmore { color: var(--muted); font-size: .78rem; cursor: pointer; }
  .fmore:hover { color: var(--text); }
  .fclear { font-size: .78rem; margin-left: .3rem; }
  .fitems { display: contents; }
  .svc { color: var(--muted); font-style: italic; }
  a.pos .nw { white-space: nowrap; }            /* lat and lon stay whole; the line may break between them */
  .posline { margin: .2rem 0 .6rem; }
  .trkbox { margin: .3rem 0 .5rem; }
  .tmap { position: relative; overflow: hidden; background: #1f2327; border: 1px solid var(--comp-bd); border-radius: 4px;
          height: 230px; cursor: grab; touch-action: none; user-select: none; }
  .tmap.drag { cursor: grabbing; }
  #mwrap .tmap { height: 640px; border: 0; border-radius: 0; }
  .tm-tiles img { position: absolute; width: 256px; height: 256px; max-width: none; pointer-events: none; }
  .tm-ov { position: absolute; left: 0; top: 0; pointer-events: none; overflow: visible; }
  .tm-ov .tm-pt { pointer-events: auto; cursor: pointer; stroke: #12171b; stroke-width: 1.5; }
  .tm-ov .tm-line { fill: none; stroke-width: 2.5; stroke-linejoin: round; stroke-linecap: round; opacity: .85; }
  .tm-ov .tm-line.sel { stroke-width: 4; opacity: 1; }
  /* A path's wide invisible twin takes the clicks / hovers; its first fix
     gets a small ring (the latest is the labelled dot). Hovering a radio's
     path or dot -- or selecting it -- fades everyone else's. */
  .tm-ov .tm-hit { fill: none; stroke: transparent; stroke-width: 14; pointer-events: stroke; cursor: pointer; }
  .tm-ov .tm-start { fill: #12171b; stroke-width: 2; }
  .tm-ov .tm-line, .tm-ov .tm-pt, .tm-ov .tm-start, .tm-ov text { transition: opacity .12s; }
  .tm-ov.hl .tm-line:not(.on), .tm-ov.hl .tm-start:not(.on) { opacity: .15; }
  .tm-ov.hl .tm-pt:not(.on), .tm-ov.hl text:not(.on) { opacity: .3; }
  .tm-ov.hl .tm-line.on { stroke-width: 4.5; opacity: 1; }
  .tm-ov text { font: 600 11px 'Helvetica Neue', Arial, sans-serif; fill: #fff; paint-order: stroke; stroke: #12171b; stroke-width: 3px; }
  .tm-zoom { position: absolute; left: 8px; top: 8px; display: flex; flex-direction: column; gap: 4px; z-index: 2; }
  .tm-zoom .btn { padding: .1rem .5rem; font-size: .95rem; line-height: 1.2; }
  .tm-att { position: absolute; right: 0; bottom: 0; background: rgba(255,255,255,.75); color: #333; font-size: .66rem;
            padding: 1px 5px; z-index: 2; max-width: 100%; }
  .tm-msg { position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%); color: var(--muted); font-size: .8rem;
            background: rgba(31,35,39,.85); padding: .3rem .6rem; border-radius: 4px; z-index: 2; text-align: center; }
  .trk { display: block; background: #1f2327; border: 1px solid var(--comp-bd); border-radius: 4px; max-height: 170px; }
  .trkline { fill: none; stroke: var(--info); stroke-width: 1.5; stroke-linejoin: round; opacity: .8; }
  .trkpt { fill: var(--info); }
  .trkstart { fill: #5cb85c; stroke: #12171b; }
  .trkend { fill: #d9534f; stroke: #12171b; }
  .fadd { display: inline-flex; gap: .3rem; align-items: center; margin-left: .3rem; flex: none; }
  .fin { background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd); border-radius: 4px;
         padding: .2rem .5rem; font: inherit; font-size: .8rem; width: 10.5rem; }
  .chip { display: inline-flex; align-items: center; gap: .35rem; cursor: pointer; max-width: 22rem;
          background: var(--panel2); border: 1px solid var(--comp-bd); border-radius: 12px; padding: .15rem .65rem;
          font-size: .8rem; color: var(--text); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .chip:hover { border-color: #5a6067; }
  .chip.active { background: #2c4a56; border-color: var(--info); color: var(--heading); }
  .chip.excl { border-color: #a94442; border-style: dashed; color: var(--muted); text-decoration: line-through; }
  .chip.excl .sw { opacity: .45; }
  .btn.sm { padding: .2rem .65rem; font-size: .8rem; }
  .chip .n { color: var(--muted); font-variant-numeric: tabular-nums; }
  .sw { display: inline-block; width: .65rem; height: .65rem; border-radius: 2px; flex: none; }
  .layout { display: grid; grid-template-columns: minmax(0, 1fr) 360px; gap: 1rem; align-items: start; }
  @media (max-width: 1300px) {                 /* details beside the table: give the table room */
    .layout { grid-template-columns: minmax(0, 1fr) 300px; }
  }
  @media (max-width: 1050px) { .layout { grid-template-columns: minmax(0, 1fr); } }
  .panel { background: var(--panel); border: 1px solid var(--comp-bd); border-radius: 4px; overflow: hidden;
           box-shadow: inset 0 1px 0 rgba(255,255,255,.05); }
  .scroll { max-height: 68vh; overflow: auto; }
  table { border-collapse: collapse; width: 100%; font-size: .86rem; }
  th, td { text-align: left; padding: .42rem .7rem; vertical-align: middle; }
  @media (max-width: 1300px) { th, td { padding-left: .5rem; padding-right: .5rem; } }
  thead th { position: sticky; top: 0; z-index: 1; background-image: linear-gradient(#41474d, #3a3f44);
             color: var(--heading); font-weight: 500; font-size: .7rem; text-transform: uppercase;
             letter-spacing: .05em; border-bottom: 2px solid var(--table-bd); white-space: nowrap; }
  th.sortable { cursor: pointer; }
  th.sortable:hover { color: var(--info); }
  tbody tr { border-top: 1px solid var(--table-bd); }
  tbody tr.alt { background: rgba(255,255,255,.035); }
  tbody tr:hover, tbody tr:hover + tr.sub { background: rgba(255,255,255,.075); }
  tr.click { cursor: pointer; }
  tr.sel, tr.sel.alt { background: rgba(91,192,222,.14); }
  /* A row's own line under it (a call's transcript): as wide as the table. */
  tbody tr.sub { border-top: 0; }
  tbody tr.sub td { padding-top: 0; white-space: normal; overflow-wrap: anywhere; }
  tbody tr.hassub td { padding-bottom: .2rem; }
  td.num { text-align: right; font-variant-numeric: tabular-nums; }
  td.mono, .mono { font-family: Menlo, Monaco, Consolas, 'Courier New', monospace; font-size: .82rem; }
  /* Encryption keyring (details panel). */
  .keylst { list-style: none; margin: .2rem 0 0; padding: 0; }
  .keylst li { padding: .35rem 0; border-top: 1px solid var(--table-bd); }
  .keylst li:first-child { border-top: 0; }
  .keyhdr { display: flex; align-items: baseline; gap: .5rem; flex-wrap: wrap; }
  .keyhdr .c { margin-left: auto; color: var(--muted); font-size: .8rem; white-space: nowrap; }
  .keyok { color: #58c389; font-size: .78rem; font-weight: 600; white-space: nowrap; }
  .keyact { margin-top: .3rem; display: flex; gap: .4rem; align-items: center; flex-wrap: wrap; }
  .keyact .ki { flex: 1 1 11rem; min-width: 8rem; font-family: Menlo, Monaco, Consolas, monospace; font-size: .8rem;
                padding: .25rem .4rem; background: var(--input-bg, #11161b); color: var(--text);
                border: 1px solid var(--table-bd); border-radius: 3px; }
  .keyact .kh { color: var(--muted); font-size: .74rem; flex-basis: 100%; }
  a.keyrm { color: var(--muted); font-size: .78rem; cursor: pointer; }
  a.keyrm:hover { color: var(--text); }
  td.wrap { white-space: pre-wrap; word-break: break-word; }
  /* Calls: a call with many badges (VOICE GROUP EMERGENCY ENCRYPTED) wraps
     them instead of widening the column for every row, and the Content column
     (the one that wraps) keeps a readable width. */
  td.ctype { white-space: normal; min-width: 7.5rem; line-height: 1.9; }
  #t-calls td.wrap { min-width: 12rem; }
  @media (max-width: 1480px) { #t-calls td.wrap { min-width: 7rem; } }
  td.nowrap { white-space: nowrap; }
  .empty { color: var(--muted); padding: 1rem .85rem; }
  .more { color: var(--muted); font-size: .8rem; padding: .5rem .85rem; border-top: 1px solid var(--table-bd); }
  .ent { color: var(--heading); cursor: pointer; font-family: Menlo, Monaco, Consolas, monospace; font-size: .84rem; }
  .ent:hover { color: var(--info); text-decoration: none; }
  .alias { color: var(--muted); font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif; font-size: .8rem; }
  .netc { display: inline-flex; align-items: center; gap: .35rem; max-width: 15rem; overflow: hidden;
          text-overflow: ellipsis; white-space: nowrap; cursor: pointer; vertical-align: bottom; }
  .badge { display: inline-block; font-size: .66rem; font-weight: 600; letter-spacing: .04em; border-radius: 3px;
           padding: .05rem .38rem; margin: 0 .25rem .1rem 0; border: 1px solid transparent; white-space: nowrap; }
  .b-voice { background: rgba(91,192,222,.18); color: var(--info); }
  .b-data { background: rgba(179,139,255,.18); color: var(--purple); }
  .b-priv { border-color: var(--warn); color: var(--warn); }
  .b-group { background: rgba(255,255,255,.07); color: var(--muted); }
  .b-emerg { background: var(--danger); color: #fff; }
  .b-enc { background: rgba(248,148,6,.2); color: var(--warn); }
  .b-strong { background: rgba(98,196,98,.18); color: var(--success); }
  .b-weak { background: rgba(248,148,6,.18); color: var(--warn); }
  .b-channel { background: rgba(91,192,222,.16); color: var(--info); }
  .b-none { background: rgba(255,255,255,.07); color: var(--muted); }
  .livecell { color: var(--success); white-space: nowrap; font-variant-numeric: tabular-nums; }
  .dot { display: inline-block; width: .55rem; height: .55rem; border-radius: 50%; margin-right: .35rem;
         background: var(--success); box-shadow: 0 0 6px rgba(98,196,98,.7); animation: pulse 1.2s ease-in-out infinite; }
  @keyframes pulse { 50% { opacity: .35; } }
  @media (prefers-reduced-motion: reduce) { .dot { animation: none; } }
  .side { position: sticky; top: .8rem; max-height: calc(100vh - 1.6rem); overflow: auto; padding: .9rem 1rem; }
  .side .desel { position: absolute; top: .55rem; right: .6rem; background: none; border: 1px solid transparent;
                 border-radius: 4px; color: var(--muted); font-size: 1rem; line-height: 1; padding: .25rem .45rem; cursor: pointer; }
  .side .desel:hover { color: var(--heading); border-color: var(--comp-bd); background: var(--panel2); }
  .side.has-sel h3:first-of-type { padding-right: 2rem; }
  .side h3 { margin: 0 0 .15rem; color: var(--heading); font-size: 1.15rem; font-weight: 500; word-break: break-word; }
  .side h4 { margin: 1rem 0 .4rem; color: var(--muted); font-size: .7rem; text-transform: uppercase;
             letter-spacing: .06em; font-weight: 500; }
  .side .hint { color: var(--muted); }
  .kv { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: .5rem; margin-top: .7rem; }
  .kv div { background: var(--panel2); border: 1px solid var(--comp-bd); border-radius: 3px; padding: .35rem .5rem; }
  .kv b { display: block; color: var(--heading); font-weight: 500; font-variant-numeric: tabular-nums; }
  .kv span { color: var(--muted); font-size: .68rem; text-transform: uppercase; letter-spacing: .05em; }
  .lst { list-style: none; margin: 0; padding: 0; }
  .lst li { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: .5rem; align-items: center;
            padding: .22rem 0; border-bottom: 1px solid rgba(0,0,0,.25); }
  .lst li .c { color: var(--muted); font-variant-numeric: tabular-nums; font-size: .8rem; }
  .bar { height: 3px; background: var(--info); border-radius: 2px; margin-top: 2px; opacity: .7; }
  .tagrow { display: flex; flex-wrap: wrap; gap: .3rem; margin-top: .4rem; }
  .mrow { display: flex; align-items: center; gap: .5rem; padding: .22rem 0; border-bottom: 1px solid rgba(0,0,0,.25); }
  .mrow .ml { flex: 1; min-width: 0; overflow-wrap: anywhere; }
  .mpick { margin-top: .7rem; align-items: center; }
  .msel { flex: 1; min-width: 0; max-width: 100%; background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd);
          border-radius: 3px; font: inherit; font-size: .85rem; padding: .15rem .2rem; }
  .ids { font-family: Menlo, Monaco, Consolas, monospace; font-size: .8rem; color: var(--text); }
  .grid2 { display: grid; grid-template-columns: repeat(auto-fill, minmax(min(330px, 100%), 1fr)); gap: .8rem; padding: .8rem; }
  .comm { background: var(--panel2); border: 1px solid var(--comp-bd); border-radius: 4px; padding: .6rem .75rem; }
  .comm h5 { margin: 0 0 .35rem; color: var(--heading); font-size: .85rem; font-weight: 500; }
  .comm .meta { color: var(--muted); font-size: .75rem; margin-bottom: .4rem; }
  .comm.same h5 { display: flex; flex-wrap: wrap; align-items: center; gap: .3rem; }
  .comm.same .amp { color: var(--muted); }
  .tier { margin-left: auto; font-size: .68rem; text-transform: uppercase; letter-spacing: .05em; padding: .05rem .35rem;
          border-radius: 3px; border: 1px solid var(--comp-bd); color: var(--muted); }
  .tier.t3 { color: #62c462; border-color: #62c462; }
  .tier.t2 { color: #e6c229; border-color: #e6c229; }
  .why { margin: 0 0 .5rem; padding-left: 1.1rem; color: var(--text); font-size: .78rem; }
  .why li.against { color: #f89406; }
  .comm .ents { display: flex; flex-wrap: wrap; gap: .25rem .6rem; }
  .section { padding: .8rem .9rem 0; color: var(--muted); font-size: .72rem; text-transform: uppercase; letter-spacing: .06em; }
  .note { color: var(--muted); font-size: .8rem; padding: .3rem .9rem .2rem; }
  .gbar { display: flex; flex-wrap: wrap; gap: .7rem; align-items: center; padding: .5rem .8rem;
          border-bottom: 1px solid var(--table-bd); font-size: .8rem; }
  .gbar label { display: inline-flex; gap: .3rem; align-items: center; cursor: pointer; }
  .gbar select { background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd); border-radius: 3px; font: inherit; }
  #gwrap { position: relative; height: 640px; background: radial-gradient(ellipse at center, #30353a 0%, #272b30 75%); }
  #gsvg { width: 100%; height: 100%; display: block; cursor: grab; touch-action: none; }
  #gsvg.panning { cursor: grabbing; }
  .glink { stroke: #8a929a; stroke-opacity: .38; }
  .glink.priv { stroke: var(--warn); stroke-dasharray: 4 3; stroke-opacity: .6; }
  .gnode { cursor: pointer; }
  .gnode .shape { stroke: #1c1e22; stroke-width: 1.5; }
  .gnode.multi .shape { stroke: #fff; stroke-dasharray: 3 2; }
  .gnode text { fill: #d7dadd; font-size: 10px; pointer-events: none; paint-order: stroke;
                stroke: #272b30; stroke-width: 3px; }
  .gnode.tg text { fill: #fff; font-weight: 600; }
  svg.focus .gnode, svg.focus .glink { opacity: .15; }
  svg.focus .gnode.hl, svg.focus .glink.hl { opacity: 1; }
  .gnode.sel .shape { stroke: var(--info); stroke-width: 3; stroke-dasharray: none; }
  .legend { display: flex; flex-wrap: wrap; gap: .9rem; padding: .5rem .8rem; color: var(--muted); font-size: .78rem;
            border-top: 1px solid var(--table-bd); align-items: center; }
  .legend svg { vertical-align: middle; margin-right: .25rem; }
  .emptybig { text-align: center; color: var(--muted); padding: 4rem 1rem; }
  .emptybig b { color: var(--heading); display: block; font-size: 1.1rem; margin-bottom: .4rem; font-weight: 500; }
  [hidden] { display: none !important; }

  /* ---- small screens and touch (the desktop layout above is unchanged) ---- */
  .acts { display: contents; }
  #more, .sheet-x, .sortbar, .clist { display: none; }
  .toast { position: fixed; left: 50%; bottom: 1.2rem; transform: translateX(-50%); z-index: 50; max-width: min(30rem, 90vw);
           background: #1f2327; color: var(--heading); border: 1px solid var(--info); border-radius: 6px;
           padding: .6rem .9rem; font-size: .85rem; box-shadow: 0 6px 18px rgba(0,0,0,.5); }
  .gzoom { display: inline-flex; gap: .3rem; }
  .play { appearance: none; cursor: pointer; display: inline-flex; align-items: center; gap: .3rem; font: inherit;
          font-size: .78rem; color: var(--info); background: rgba(91,192,222,.12); border: 1px solid rgba(91,192,222,.35);
          border-radius: 12px; padding: .05rem .55rem; white-space: nowrap; font-variant-numeric: tabular-nums; }
  .play:hover { background: rgba(91,192,222,.22); }
  .play.on { color: #fff; background: var(--info); border-color: var(--info); }
  @media (pointer: coarse) { .play { min-height: 36px; padding: .2rem .75rem; font-size: .85rem; } }
  .dl { display: inline-block; margin-left: .35rem; color: var(--muted); text-decoration: none; font-size: .9rem; padding: 0 .15rem; }
  .dl:hover { color: var(--info); text-decoration: none; }
  @media (pointer: coarse) { .dl { padding: .3rem .5rem; font-size: 1.05rem; } }
  .stt { display: block; color: #e3e6e8; font-style: italic; white-space: normal; }
  .stt::before { content: '\201C'; } .stt::after { content: '\201D'; }
  td.tgcell { white-space: nowrap; }
  /* Calls toolbar: pause the list, audio filter, speech-to-text settings, zip. */
  .callbar { display: flex; flex-wrap: wrap; align-items: center; gap: .45rem .9rem; padding: .45rem .8rem;
             border-bottom: 1px solid var(--table-bd); font-size: .8rem; color: var(--muted); }
  .callbar label { display: inline-flex; gap: .3rem; align-items: center; cursor: pointer; }
  .callbar select { background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd); border-radius: 3px; font: inherit; }
  .callbar .btn { padding: .2rem .65rem; font-size: .78rem; }
  .callbar .grow { flex: 1 1 auto; }
  body.filemode .callbar, .callbar .audctl.off { display: none !important; }
  @media (pointer: coarse) {
    .callbar .btn { min-height: 40px; }
    .callbar select, .callbar label { min-height: 40px; }
    .callbar input[type=checkbox] { width: 20px; height: 20px; }
  }
  /* Now playing: the call, its progress and its transcript, pinned to the
     bottom of the window so a busy list can't scroll it away. */
  .np { position: fixed; left: 0; right: 0; bottom: 0; z-index: 40; background: #1f2327; border-top: 1px solid var(--info);
        box-shadow: 0 -6px 18px rgba(0,0,0,.45); }
  .np .in { display: flex; align-items: flex-start; gap: .7rem; max-width: 1500px; margin: 0 auto; padding: .55rem 1rem; }
  .np .main { flex: 1 1 auto; min-width: 0; }
  .np .meta { font-size: .8rem; color: var(--muted); display: flex; flex-wrap: wrap; gap: .1rem .6rem; align-items: center; }
  .np .meta b { color: var(--heading); font-weight: 500; }
  .np .tx { margin-top: .25rem; font-size: .95rem; color: var(--heading); min-height: 1.3em; overflow-wrap: anywhere; }
  .np .tx.st { color: var(--muted); font-style: italic; font-size: .85rem; }
  .np .tx a { margin-left: .4rem; }
  .np .x { appearance: none; background: none; border: 0; color: var(--muted); cursor: pointer; font-size: 1rem; padding: .2rem .4rem; }
  .np .x:hover { color: var(--heading); }
  .np .prog { height: 2px; background: var(--info); width: 0; transition: width .2s linear; }
  body.np-on { padding-bottom: 6.5rem; }
  body.np-on .toast { bottom: 7rem; }
  @media (pointer: coarse) { .np .x { padding: .5rem .7rem; } .np .dl { padding: .4rem .6rem; } }
  /* Details: a drawer over the page instead of a column below it. */
  @media (max-width: 1050px) {
    .side { position: fixed; z-index: 30; top: 0; right: 0; bottom: 0; width: min(400px, 92vw); max-height: none;
            border-radius: 0; box-shadow: -10px 0 28px rgba(0,0,0,.55); transform: translateX(105%); visibility: hidden;
            transition: transform .2s ease, visibility 0s linear .2s; overscroll-behavior: contain; }
    body.sheet .side { transform: none; visibility: visible; transition: transform .2s ease; }
    .side .desel { display: none; }
    .sheet-x { display: flex; justify-content: flex-end; position: sticky; top: -.9rem; margin: -.9rem -1rem .3rem;
               padding: .5rem .6rem; background: var(--panel); z-index: 2; }
    .netc { max-width: 11rem; }
    .side .netc { max-width: 100%; }
    td.tgcell { white-space: nowrap; }
  }
  /* Compact header (narrow windows; touch screens up to tablet size; short
     touch screens such as a phone held sideways): the actions fold into a menu. */
  @media (max-width: 940px), (max-width: 1050px) and (pointer: coarse), (max-height: 500px) and (pointer: coarse) {
    .hdr { gap: .6rem; padding-top: .6rem; padding-bottom: .6rem; flex-wrap: nowrap; }
    .ttl { flex: none; }
    h1 { font-size: 1.15rem; white-space: nowrap; }
    .sub .desc { display: none; }
    .hdr-actions { flex: 1 1 auto; justify-content: flex-end; flex-wrap: nowrap; position: relative; min-width: 0; }
    .hdr-actions input[type=search] { flex: 0 1 20rem; width: auto; min-width: 0; }
    .recst { max-width: 40vw; }
    #more { display: inline-block; flex: none; }
    .acts { display: none; }
    .acts.open { display: flex; flex-direction: column; align-items: stretch; gap: .45rem; position: absolute; right: 0;
                 top: calc(100% + 6px); z-index: 25; min-width: 15rem; padding: .6rem; background: var(--panel);
                 border: 1px solid var(--comp-bd); border-radius: 6px; box-shadow: 0 8px 22px rgba(0,0,0,.5); }
    .acts.open .btn { text-align: left; }
    .acts.open #help .hl { display: inline; }
    .acts .dropdown { display: block; }
    .acts .dropdown .btn { width: 100%; }
    .acts .menu { position: static; min-width: 0; margin-top: .3rem; box-shadow: none; }
  }
  @media (max-width: 640px) {
    .hdr { flex-wrap: wrap; }
    .hdr-actions { width: 100%; }
    .hdr-actions input[type=search] { flex: 1 1 auto; }
  }
  /* Narrow tables: let headers and badges wrap rather than scroll sideways. */
  @media (max-width: 1400px), (pointer: coarse) {
    thead th { white-space: normal; }
    td.ids { word-break: break-word; }
  }
  @media (max-width: 900px) { th, td { padding-left: .5rem; padding-right: .5rem; } td .netc { max-width: 9.5rem; } }
  /* Compact stat cards, tabs and network chips: tablets and smaller. */
  @media (max-width: 1050px) {
    .cards { display: grid; grid-template-columns: repeat(var(--ncards, 7), minmax(0, 1fr)); gap: .5rem; }
    .card { min-width: 0; padding: .45rem .7rem; }
    .card .l { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  }
  @media (max-width: 640px), (max-height: 500px) and (pointer: coarse) {
    .tabs { flex-wrap: nowrap; overflow-x: auto; scrollbar-width: none; }
    .tabs::-webkit-scrollbar { display: none; }
    .tab { flex: none; padding: .4rem .7rem; }
    .cards { grid-template-columns: repeat(var(--ncards, 7), minmax(0, 1fr)); gap: .4rem; margin-bottom: .7rem; }
    .card { padding: .3rem .5rem; }
    .card .n { font-size: 1.15rem; }
    .card .l { font-size: .58rem; letter-spacing: .03em; }
    .filters { margin-bottom: .7rem; }
    .frow { flex-wrap: nowrap; overflow-x: auto; scrollbar-width: none; }
    .frow::-webkit-scrollbar { display: none; }
    .fhint { flex: none; }
    .chip { flex: none; }
  }
  /* Phones. */
  @media (max-width: 640px) {
    .wrap { padding: 0 .75rem; }
    header { margin-bottom: .7rem; }
    .cards { grid-template-columns: repeat(4, minmax(0, 1fr)); }
    /* Tables become stacked cards: the first column is the title, the rest
       label / value pairs. A sort menu replaces the column headers. */
    .sortbar { display: flex; gap: .5rem; align-items: center; padding: .5rem .75rem; border-bottom: 1px solid var(--table-bd);
               color: var(--muted); font-size: .8rem; }
    .sortbar select { background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd); border-radius: 3px; font: inherit; }
    .scroll table, .scroll tbody { display: block; }
    .scroll thead { display: none; }
    .scroll tbody tr { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: .3rem .9rem; padding: .6rem .75rem; }
    .scroll td { display: block; padding: 0; text-align: left; min-width: 0; overflow-wrap: anywhere; }
    .scroll td.num { text-align: left; }
    .scroll td::before { content: attr(data-label); display: block; color: var(--muted); font-size: .6rem;
                         text-transform: uppercase; letter-spacing: .05em; }
    .scroll td:first-child { grid-column: 1 / -1; font-size: 1rem; }
    .scroll td:first-child::before, .scroll td.empty::before { display: none; }
    .scroll td.empty { grid-column: 1 / -1; }
    .netc { max-width: 100%; }
    /* Calls: one compact card per call. */
    .clist { display: block; }
    .ccard { padding: .6rem .75rem; border-top: 1px solid var(--table-bd); }
    .ccard:nth-child(even) { background: rgba(255,255,255,.035); }
    .ccard .r1 { display: flex; flex-wrap: wrap; align-items: center; gap: .25rem .5rem; font-size: .8rem; color: var(--muted); }
    .ccard .r1 .t { color: var(--text); }
    .ccard .r2 { margin: .3rem 0 .2rem; font-size: .95rem; }
    .ccard .r3 { display: flex; flex-wrap: wrap; gap: .2rem .6rem; align-items: center; font-size: .8rem; color: var(--muted); }
    .ccard .tx { margin-top: .3rem; white-space: pre-wrap; word-break: break-word; font-size: .85rem; }
    /* Details: a bottom sheet. */
    .side { top: auto; left: 0; width: auto; max-height: 72vh; border-radius: 12px 12px 0 0;
            box-shadow: 0 -10px 28px rgba(0,0,0,.55); transform: translateY(105%); }
    .gbar { gap: .5rem; }
  }
  /* Touch screens: finger-sized targets, one scroll (the page), and a graph
     that leaves room to scroll past it. */
  @media (pointer: coarse) {
    .btn { min-height: 44px; padding: .45rem .95rem; font-size: .85rem; }
    .tab { min-height: 44px; }
    .chip { min-height: 38px; padding: .3rem .8rem; font-size: .85rem; }
    input[type=search] { min-height: 44px; font-size: 16px; }
    .menu a { padding: .75rem .9rem; }
    th, td { padding-top: .65rem; padding-bottom: .65rem; }
    .ent { display: inline-block; padding: .35rem .1rem; }
    .netc { padding: .3rem 0; }
    .scroll { max-height: none !important; overflow-y: visible; overflow-x: auto; }
    .lst li { padding: .45rem 0; }
    .gbar select, .sortbar select, .msel { min-height: 40px; }
    .gbar label { min-height: 40px; }
    .gbar input[type=checkbox] { width: 20px; height: 20px; }
    #gwrap { height: min(640px, max(320px, 66vh)); }
    #mwrap .tmap { height: min(640px, max(320px, 66vh)); }
    .filebar a, .filebar .x { display: inline-block; padding: .45rem .2rem; }
  }
</style>
</head>
<body>
<header><div class="wrap hdr">
  <div class="ttl">
    <h1><span class="accent">dsd-server</span> network explorer</h1>
    <div class="sub"><span id="live">connecting&hellip;</span><span class="desc"> &middot; calls, talkgroups &amp; radios, associated per protocol</span>
      &middot; <a href="/">status page</a></div>
  </div>
  <div class="hdr-actions">
    <input id="q" type="search" placeholder="Find radio, talkgroup, alias, text&hellip;" autocomplete="off">
    <span id="recst" class="recst live-only dev-only"></span>
    <button id="more" class="btn" type="button" aria-haspopup="true" aria-expanded="false" title="Actions">&#9776; Menu</button>
    <div class="acts" id="acts">
    <button id="rec" class="btn live-only dev-only" type="button" title="Record everything the explorer receives, to replay and analyse offline">&#9679; Record</button>
    <button id="aud" class="btn live-only" type="button" title="Record each call's decoded voice, to play back here (off by default)">&#9835; Audio</button>
    <button id="pause" class="btn live-only" type="button">Pause</button>
    <button id="clear" class="btn live-only" type="button" title="Forget everything learned so far">Clear</button>
    <span class="dropdown live-only">
      <button id="exp" class="btn" type="button" title="Save what the explorer shows">Export &#9662;</button>
      <div id="expmenu" class="menu" hidden>
        <a href="/net/export.json" download>Explorer data (.json)<small>re-open later here with Open&hellip;</small></a>
        <a href="#" id="exp-audio">Explorer data with audio (.zip)<small>the same, plus the calls&rsquo; audio &middot; Open&hellip; plays it</small></a>
        <a href="/net/export.graphml" download>Association graph (.graphml)<small>Gephi &middot; Cytoscape &middot; yEd &middot; networkx</small></a>
        <a href="#" id="exp-kml">Positions (.kml)<small>the listed radios' position reports &middot; Google Earth &middot; My Maps</small></a>
      </div>
    </span>
    <button id="import" class="btn live-only" type="button" title="Add saved exports (e.g. from other receivers) to the live view">Import&hellip;</button>
    <input type="file" id="importfile" accept=".json,.gz,.zip,application/json,application/zip" multiple hidden>
    <button id="open" class="btn" type="button" title="View saved explorer exports -- several are merged into one view (or drop them on the page)">Open&hellip;</button>
    <input type="file" id="openfile" accept=".json,.gz,.zip,application/json,application/zip" multiple hidden>
    <a id="help" class="btn" href="/net/manual.pdf" target="_blank" rel="noopener" title="Help: the explorer's user manual (PDF, opens in a new tab)" aria-label="Help: user manual">?<span class="hl"> Help</span></a>
    </div>
  </div>
</div></header>
<div class="wrap">
  <div id="filebar" class="filebar" hidden></div>
  <div id="impbar" class="filebar imp live-only" hidden></div>
  <div id="impnote" class="filebar imp live-only" hidden></div>
  <div class="tabs famtabs" id="famtabs"></div>
  <div id="empty" class="emptybig"><b>Waiting for digital-voice traffic</b>
    Start a decode session (DMR, P25, NXDN, dPMR, D-STAR, YSF, TETRA, EDACS) and its calls, talkgroups,
    radios and networks will appear here as they are heard. Associations are kept per protocol.</div>
  <div id="main" hidden>
    <div class="cards" id="cards"></div>
    <div class="filters" id="filters"></div>
    <div class="layout">
      <div>
        <div class="tabs" id="viewtabs">
          <button class="tab" data-v="calls">Calls <span class="count" id="c-calls"></span></button>
          <button class="tab" data-v="tgs">Talkgroups <span class="count" id="c-tgs"></span></button>
          <button class="tab" data-v="radios">Radios <span class="count" id="c-radios"></span></button>
          <button class="tab" data-v="graph">Graph</button>
          <button class="tab" data-v="map">Map</button>
          <button class="tab" data-v="links">Links</button>
          <button class="tab" data-v="nets">Networks <span class="count" id="c-nets"></span></button>
        </div>
        <div class="panel" id="v-calls">
          <div class="callbar">
            <label class="audctl" title="List only calls whose voice was recorded"><input type="checkbox" id="audonly"> With audio only</label>
            <span class="grow"></span>
            <label class="audctl live-only" id="asrctl" title="Turn each call's speech into text when you play it (runs in this browser)"><input type="checkbox" id="asron"> Transcribe on play</label>
            <select id="asrlang" class="audctl live-only" aria-label="Spoken language" title="Spoken language"></select>
            <select id="asrmodel" class="audctl live-only" aria-label="Speech-to-text model" title="Speech-to-text model: larger is more accurate but slower" hidden></select>
            <button id="zip" class="btn audctl live-only" type="button" title="Download the audio of the listed calls (up to 25 MB), with a calls.csv of who, when and what was said">&#10515; Audio (.zip)</button>
          </div>
          <div class="scroll" id="t-calls"></div>
        </div>
        <div class="panel" id="v-tgs" hidden><div class="scroll" id="t-tgs"></div></div>
        <div class="panel" id="v-radios" hidden><div class="scroll" id="t-radios"></div></div>
        <div class="panel" id="v-graph" hidden>
          <div class="gbar">
            <label>Nodes <select id="g-cap"><option>100</option><option selected>250</option><option>500</option><option>1000</option></select></label>
            <label><input type="checkbox" id="g-priv" checked> Private-call links</label>
            <label><input type="checkbox" id="g-labels" checked> Radio labels</label>
            <span class="gzoom"><button class="btn" id="g-zout" type="button" title="Zoom out" aria-label="Zoom out">&minus;</button><button class="btn" id="g-zin" type="button" title="Zoom in" aria-label="Zoom in">+</button></span>
            <button class="btn" id="g-fit" type="button" title="Show the whole graph -- and keep it in view as it grows, until you zoom or pan">Fit</button>
            <span id="g-note" style="color:var(--muted)"></span>
          </div>
          <div id="gwrap"><svg id="gsvg"></svg></div>
          <div class="legend" id="g-legend"></div>
        </div>
        <div class="panel" id="v-map" hidden>
          <div class="gbar">
            <label>Map <select id="m-tiles"></select></label>
            <label><input type="checkbox" id="m-paths" checked> Paths</label>
            <button class="btn" id="m-fit" type="button">Fit</button>
            <span id="m-note" style="color:var(--muted)"></span>
          </div>
          <div id="mwrap"></div>
        </div>
        <div class="panel" id="v-links" hidden><div class="scroll" id="t-links" style="max-height:78vh"></div></div>
        <div class="panel" id="v-nets" hidden><div class="scroll" id="t-nets"></div></div>
      </div>
      <aside class="panel side" id="detail"></aside>
    </div>
  </div>
</div>
<div id="np" class="np" hidden>
  <div class="prog" id="npprog"></div>
  <div class="in">
    <button id="npplay" class="play on" type="button" title="Play / stop"><span>&#9632;</span></button>
    <div class="main">
      <div class="meta" id="npmeta"></div>
      <div class="tx" id="nptx"></div>
    </div>
    <a id="npdl" class="dl" href="#" download title="Download this call's audio">&#10515;</a>
    <button id="npx" class="x" type="button" title="Close" aria-label="Close">&#10005;</button>
  </div>
</div>
<script>
(function () {
'use strict';
var POLL = 1500, LIVE_MS = 3000, ROWS = 400;
var FAMN = {dmr:'DMR', p25:'P25', nxdn:'NXDN', tetra:'TETRA', dpmr:'dPMR', dstar:'D-STAR', ysf:'YSF',
            edacs:'EDACS / ProVoice', x2tdma:'X2-TDMA'};
var PAL = ['#5bc0de','#62c462','#f89406','#ee5f5b','#b38bff','#e6c229','#3fc1a5','#ff7eb6','#8fa8ff',
           '#c3e88d','#ffab70','#4dd0e1','#d4a5ff','#a3d977'];
var S = { d: null, fam: null, nets: {}, tgf: {}, rf: {}, view: 'calls', sel: null, q: '', paused: false, skew: 0,
          ncol: {}, nidx: {}, sort: {}, rows: {}, audOnly: false, fOpen: false };
var IX = null;

function $(id) { return document.getElementById(id); }
function h(tag, a, kids) {
  var e = document.createElement(tag);
  if (a) for (var k in a) {
    var v = a[k];
    if (v == null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v === true ? '' : v);
  }
  if (kids != null) {
    if (!Array.isArray(kids)) kids = [kids];
    for (var i = 0; i < kids.length; i++) {
      var c = kids[i];
      if (c == null || c === false) continue;
      e.appendChild(typeof c === 'object' ? c : document.createTextNode(String(c)));
    }
  }
  return e;
}
function store(k, v) { try { localStorage.setItem('netx.' + k, v); } catch (e) {} }
function load(k) { try { return localStorage.getItem('netx.' + k); } catch (e) { return null; } }
// In file view the clock stands still at the export moment, so "3m ago"
// means three minutes before the export.
function now() { return S.file ? S.d.now : Date.now() + S.skew; }
function p2(n) { return (n < 10 ? '0' : '') + n; }
function hms(ms) { var d = new Date(ms); return p2(d.getUTCHours()) + ':' + p2(d.getUTCMinutes()) + ':' + p2(d.getUTCSeconds()); }
function dt(ms) { var d = new Date(ms); return d.getUTCFullYear() + '-' + p2(d.getUTCMonth() + 1) + '-' + p2(d.getUTCDate()) + ' ' + hms(ms) + 'Z'; }
function ago(ms) {
  if (!ms) return '-';
  var s = Math.max(0, Math.round((now() - ms) / 1000));
  if (s < 60) return s + 's ago';
  if (s < 3600) return Math.floor(s / 60) + 'm ago';
  if (s < 86400) return Math.floor(s / 3600) + 'h ago';
  return Math.floor(s / 86400) + 'd ago';
}
function dur(ms) {
  var s = Math.max(0, ms) / 1000;
  if (s < 1) return '<1s';
  if (s < 10) return s.toFixed(1) + 's';
  if (s < 60) return Math.round(s) + 's';
  var m = Math.floor(s / 60);
  return m + 'm ' + p2(Math.round(s - m * 60)) + 's';
}
function keys(o) { return o ? Object.keys(o) : []; }
function sum(o) { var t = 0; for (var k in o) t += o[k]; return t; }
function live(c) { return !S.file && c.open && now() - c.last < LIVE_MS; }

// ---------- network merges ----------
// d.merges (per protocol: network key -> the key it is shown under) are the
// server's shared "these are one network" rules -- e.g. one channel heard at
// two frequency offsets. applyMerges shows each group as one network (its
// parts in .parts) that every talkgroup, radio and call refers to. The data
// underneath stays apart (kept in networks0 / networks0 / net0), so it can be
// re-applied whenever the rules change, and Unmerge just drops a rule.
var CONF_RANK = { strong: 3, channel: 2, weak: 1, none: 0 };
function applyMerges(d) {
  var M = d.merges || {};
  keys(d.families).forEach(function (f) {
    var F = d.families[f], R = M[f] || {};
    if (!F.networks0 && !keys(R).length) return;       // nothing merged, ever: leave it as it came
    if (!F.networks0) F.networks0 = F.networks;
    var root = function (k) { return R[k] || k; }, by = {}, out = [];
    F.networks0.slice().sort(function (a, b) { return (R[a.key] ? 1 : 0) - (R[b.key] ? 1 : 0); }).forEach(function (n) {
      var r = root(n.key), g = by[r];
      if (!g) {                                          // a target first, so its label names the group
        g = by[r] = Object.assign({}, n, { key: r, ids: Object.assign({}, n.ids), sites: n.sites.slice(),
                                           freqs: (n.freqs || []).slice(), keys: Object.assign({}, n.keys), parts: [n] });
        out.push(g);
        return;
      }
      g.parts.push(n);
      if ((CONF_RANK[n.confidence] || 0) > (CONF_RANK[g.confidence] || 0)) g.confidence = n.confidence;
      keys(n.ids).forEach(function (k) { if (!(k in g.ids)) g.ids[k] = n.ids[k]; });
      n.sites.forEach(function (x) { if (g.sites.indexOf(x) < 0) g.sites.push(x); });
      (n.freqs || []).forEach(function (x) { if (g.freqs.indexOf(x) < 0) g.freqs.push(x); });
      g.freqs.sort(function (a, b) { return a - b; });
      g.sessions += n.sessions; g.calls += n.calls;
      keys(n.keys || {}).forEach(function (k) { g.keys[k] = (g.keys[k] || 0) + n.keys[k]; });
      g.first = !g.first ? n.first : !n.first ? g.first : Math.min(g.first, n.first);
      g.last = Math.max(g.last, n.last);
    });
    F.networks = out;
    var remap = function (o) {
      if (!o.networks0) o.networks0 = o.networks;
      var l = [];
      o.networks0.forEach(function (k) { k = root(k); if (l.indexOf(k) < 0) l.push(k); });
      o.networks = l;
    };
    F.talkgroups.forEach(remap);
    F.radios.forEach(remap);
    F.calls.forEach(function (c) { if (c.net0 == null) c.net0 = c.net; c.net = root(c.net0); });
  });
}
// The same rules as the server's (merges_add / merges_remove), for file views.
function mergesAdd(M, f, from, to) {
  var F = M[f] = M[f] || {}, r = F[to] || to;
  if (r === from || (F[from] || from) === r) return false;
  F[from] = r;
  keys(F).forEach(function (k) { if (F[k] === from) F[k] = r; });
  return true;
}
function mergesRemove(M, f, key) {
  var F = M[f];
  if (!F) return false;
  if (F[key]) { delete F[key]; return true; }
  var ch = false;
  keys(F).forEach(function (k) { if (F[k] === key) { delete F[k]; ch = true; } });
  return ch;
}
// Merge networks `froms` into `to` (live: on the server, for everyone; file
// view: in this view only), or undo (`to` null: unmerge each of `froms`).
function changeMerges(froms, to, done) {
  var fam = S.fam;
  var after = function (merges, n) {
    S.d.merges = merges;
    applyMerges(S.d);
    if (S.sel && S.sel.type === 'net' && to && froms.indexOf(S.sel.id) >= 0) S.sel = { type: 'net', id: to };
    GR.sig = '';
    render();
    renderDetail(true);                                 // even with the pointer still on the picker
    if (done) done(n);
  };
  if (S.file) {
    var M = JSON.parse(JSON.stringify(S.d.merges || {})), n = 0;
    froms.forEach(function (k) { if (to ? mergesAdd(M, fam, k, to) : mergesRemove(M, fam, k)) ++n; });
    after(M, n);
    return;
  }
  var res = null, n = 0, chain = Promise.resolve();
  froms.forEach(function (k) {
    chain = chain.then(function () {
      var u = to ? '/net/networks/merge?fam=' + encodeURIComponent(fam) + '&from=' + encodeURIComponent(k) + '&to=' + encodeURIComponent(to)
                 : '/net/networks/unmerge?fam=' + encodeURIComponent(fam) + '&key=' + encodeURIComponent(k);
      return fetch(u, { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) { res = j.merges; if (j.ok) ++n; });
    });
  });
  chain.then(function () { if (res) after(res, n); })
       .catch(function () { toast('Could not reach the server to change the merge.'); });
}
function mergeInto(froms, to) {
  S.mpick = null;
  if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
  changeMerges(froms, to, function (n) {
    var t = IX.netByKey[to];
    toast(n ? 'Merged ' + (froms.length > 1 ? froms.length + ' networks' : 'the network') + ' into ' + (t ? t.label : to) +
              (S.file ? ' (in this file view only)' : '') + '. Unmerge undoes it.'
            : 'Nothing to merge — already one network.');
  });
}
function unmerge(keysList) {
  changeMerges(keysList, null, function (n) { toast(n ? 'Unmerged.' : 'Not merged.'); });
}
// ---- "probably the same network" ----
// Evidence that two networks of this protocol are one (a channel heard at two
// frequency offsets, say), from the data on the page. Per pair of networks
// (pairKey): twins -- the same call (source, target, kind; no more than 4 s
// apart) heard on both, by different streams: near-proof; clash -- different
// calls on the same slot at the same moment: two channels; sameStream -- one
// stream heard both at once: two channels for certain; shared talkgroups /
// radios. Worked out once per data version.
var SAME = { sig: '', ev: {} };
function pairKey(a, b) { return a < b ? a + '\n' + b : b + '\n' + a; }
function sameEvidence() {
  var sig = S.d.version + '|' + S.fam + '|' + IX.nets.length + '|' + IX.calls.length;
  if (SAME.sig === sig) return SAME.ev;
  var ev = {};
  var E = function (a, b) { var k = pairKey(a, b); return ev[k] || (ev[k] = { twins: 0, clash: 0, sameStream: 0, tgs: 0, radios: 0 }); };
  // Twins: calls with the same source, target and kind, by bucket.
  var by = {};
  IX.calls.forEach(function (c) { if (c.src && c.tgt && c.net) (by[c.src + '|' + c.tgt + '|' + (c.priv ? 1 : 0)] = by[c.src + '|' + c.tgt + '|' + (c.priv ? 1 : 0)] || []).push(c); });
  var twinOf = {};
  keys(by).forEach(function (k) {
    var L = by[k];
    if (L.length < 2) return;
    L.sort(function (a, b) { return a.start - b.start; });
    for (var i = 0; i < L.length; i++)
      for (var j = i + 1; j < L.length && L[j].start <= L[i].last + 4000; j++)
        if (L[j].net !== L[i].net && L[j].session !== L[i].session) {
          E(L[i].net, L[j].net).twins++;
          twinOf[L[i].id + '|' + L[j].id] = twinOf[L[j].id + '|' + L[i].id] = 1;
        }
  });
  // Clashes: calls on two networks at the same moment.
  var all = IX.calls.filter(function (c) { return c.net; }).sort(function (a, b) { return a.start - b.start; }), act = [];
  all.forEach(function (c) {
    act = act.filter(function (o) { return o.last >= c.start; });
    act.forEach(function (o) {
      if (o.net === c.net || twinOf[o.id + '|' + c.id]) return;
      if (o.session === c.session) E(o.net, c.net).sameStream++;
      else if (o.slot === c.slot && !(o.src === c.src && o.tgt === c.tgt)) E(o.net, c.net).clash++;
    });
    act.push(c);
  });
  // Shared talkgroups and radios.
  var share = function (list, f) {
    list.forEach(function (x) {
      var ns = x.networks;
      for (var i = 0; i < ns.length; i++) for (var j = i + 1; j < ns.length; j++) E(ns[i], ns[j])[f]++;
    });
  };
  share(IX.tgs, 'tgs');
  share(IX.radios, 'radios');
  SAME = { sig: sig, ev: ev };
  return ev;
}
// A network with no decoded identity -- only a frequency ("ch@<freq>") or not
// even that ("unknown:<stream>"); shown as "Unidentified · …". These come and
// go with their stream and carry nothing to merge on.
function unidentifiedNet(n) { return !n || !n.key || n.key.indexOf('ch@') === 0 || n.key.indexOf('unknown:') === 0; }
var CODE_NAME = { cc: 'color code', nac: 'NAC', ran: 'RAN', rpt1: 'repeater', downlink: 'downlink' };
function netCode(x) {
  var o = x.ids || {};
  for (var k in CODE_NAME) if (o[k]) return { k: k, v: o[k] };
  return null;
}
function freqGap(a, b) {
  var best = Infinity;
  (a.freqs || []).forEach(function (f) { (b.freqs || []).forEach(function (g) { best = Math.min(best, Math.abs(f - g)); }); });
  return best;
}
// How likely networks a and b are one: null when the evidence rules it out
// (both have a system id; different codes; one stream heard both at once;
// different calls on one slot at the same moment, with no call heard on both;
// channels more than 6.25 kHz apart, or unknown, with no call heard on both).
// Otherwise { tier: 3 very likely | 2 likely | 1 possible, why: [...], against: [...] }.
function sameJudge(a, b, e) {
  e = e || { twins: 0, clash: 0, sameStream: 0, tgs: 0, radios: 0 };
  // An unidentified network is never a merge candidate: nothing has been
  // decoded about it (only its frequency, "ch@<freq>" / "Unidentified · MHz",
  // or not even that, "unknown:…"), so there is no identity for it to be "the
  // same" as -- and it is transient, gone the moment its stream decodes an id
  // or drops. Suggesting it made cards flicker in and out as streams churned.
  if (unidentifiedNet(a) || unidentifiedNet(b)) return null;
  var ca = netCode(a), cb = netCode(b), gap = freqGap(a, b);
  var sameCode = ca && cb && ca.k === cb.k && ca.v === cb.v;
  if (a.confidence === 'strong' && b.confidence === 'strong') return null;
  if (ca && cb && !sameCode) return null;
  if (e.sameStream) return null;
  if (e.clash && !e.twins) return null;
  if (!e.twins && !(gap <= 6250)) return null;
  var why = [], against = [];
  if (e.twins) why.push(e.twins + ' call' + (e.twins > 1 ? 's' : '') + ' heard on both');
  if (sameCode) why.push('same ' + CODE_NAME[ca.k] + ' (' + ca.v + ')');
  if (isFinite(gap)) why.push(gap ? khz(gap) + ' apart' : 'same channel');
  var handoff = a.last && b.last && (a.last < b.first || b.last < a.first);
  if (handoff) why.push('one took over when the other stopped');
  if (e.tgs || e.radios) why.push([e.tgs ? e.tgs + ' talkgroup' + (e.tgs > 1 ? 's' : '') : '', e.radios ? e.radios + ' radio' + (e.radios > 1 ? 's' : '') : '']
                                  .filter(Boolean).join(' and ') + ' shared');
  if (e.clash) against.push('different calls at the same moment ' + e.clash + '\u00D7');
  var tier = e.twins && !e.clash ? 3 : e.twins || (sameCode && gap <= 2500 && (handoff || e.tgs || e.radios)) ? 2 : 1;
  return { tier: tier, why: why, against: against, gap: gap, twins: e.twins };
}
// Pairs the user said are not the same (per browser): protocol -> [pairKey].
function notSame() { try { return JSON.parse(load('notSame') || '{}') || {}; } catch (x) { return {}; } }
function setNotSame(a, b, on) {
  var m = notSame(), l = m[S.fam] || [], k = pairKey(a, b), i = l.indexOf(k);
  if (on && i < 0) l.push(k);
  if (!on && i >= 0) l.splice(i, 1);
  m[S.fam] = l;
  store('notSame', JSON.stringify(m));
}
// Every likely pair, likeliest first ({ a, b, j }; a the busier), and how
// many were set aside with Not the same.
function sameSuggestions() {
  var ev = sameEvidence(), hidden = notSame()[S.fam] || [], out = [], nh = 0;
  var ns = IX.nets;
  for (var i = 0; i < ns.length; i++)
    for (var k = i + 1; k < ns.length; k++) {
      var a = ns[i], b = ns[k], pk = pairKey(a.key, b.key);
      var j = sameJudge(a, b, ev[pk]);
      if (!j) continue;
      if (hidden.indexOf(pk) >= 0) { ++nh; continue; }
      out.push(a.calls >= b.calls ? { a: a, b: b, j: j } : { a: b, b: a, j: j });
    }
  out.sort(function (x, y) { return (y.j.tier - x.j.tier) || (y.j.twins - x.j.twins) || (x.j.gap - y.j.gap) || (y.a.calls + y.b.calls - x.a.calls - x.b.calls); });
  return { list: out, hidden: nh };
}
var TIER = { 3: 'very likely', 2: 'likely', 1: 'possible' };
// The Links view's "Probably the same network" cards.
function sameSection(cont) {
  var sg = sameSuggestions();
  if (!sg.list.length && !sg.hidden) return false;
  cont.appendChild(h('div', { class: 'section', text: 'Probably the same network — merge suggestions (' + sg.list.length + ')' }));
  cont.appendChild(h('div', { class: 'note', text: 'Networks that look like one channel heard at different frequency offsets (or by different receivers). ' +
    'Nothing is merged until you press Merge' + (S.file ? ' (in this file view only)' : ' (for everyone viewing this server; Unmerge in the network’s details undoes it)') + '.' }));
  var grid = h('div', { class: 'grid2' });
  sg.list.slice(0, 40).forEach(function (x) {
    grid.appendChild(h('div', { class: 'comm same' }, [
      h('h5', null, [netc(x.a.key), h('span', { class: 'amp', text: ' \u21C4 ' }), netc(x.b.key),
                     h('span', { class: 'tier t' + x.j.tier, text: TIER[x.j.tier] })]),
      h('ul', { class: 'why' }, x.j.why.map(function (w) { return h('li', { text: w }); })
        .concat(x.j.against.map(function (w) { return h('li', { class: 'against', text: 'but: ' + w }); }))),
      h('div', { class: 'tagrow' }, [
        h('button', { class: 'btn sm', type: 'button', title: 'Show ' + x.b.label + ' as part of ' + x.a.label + ' (the busier one)',
                      onclick: function () { mergeInto([x.b.key], x.a.key); } }, 'Merge'),
        h('button', { class: 'btn sm', type: 'button', title: 'Stop suggesting this pair (in this browser)',
                      onclick: function () { setNotSame(x.a.key, x.b.key, true); renderView(); } }, 'Not the same')])]));
  });
  cont.appendChild(grid);
  if (sg.hidden)
    cont.appendChild(h('div', { class: 'note' }, [sg.hidden + ' pair' + (sg.hidden > 1 ? 's' : '') + ' marked “not the same”. ',
      h('a', { href: '#', onclick: function (e) { e.preventDefault(); var m = notSame(); delete m[S.fam]; store('notSame', JSON.stringify(m)); renderView(); } },
        'Suggest them again')]));
  return true;
}
// For the details' Merge into… picker: the other networks, likeliest first.
function mergeCandidates(n) {
  var ev = sameEvidence();
  return IX.nets.filter(function (o) { return o.key !== n.key; }).map(function (o) {
    var j = sameJudge(n, o, ev[pairKey(n.key, o.key)]), cn = netCode(n), co = netCode(o);
    return { n: o, j: j, same: !!(cn && co && cn.k === co.k && cn.v === co.v), d: freqGap(n, o), s: ((ev[pairKey(n.key, o.key)] || {}).tgs || 0) + ((ev[pairKey(n.key, o.key)] || {}).radios || 0) };
  }).sort(function (a, b) { return ((b.j ? b.j.tier : 0) - (a.j ? a.j.tier : 0)) || (b.same - a.same) || (a.d - b.d) || (b.s - a.s) || (b.n.calls - a.n.calls); });
}
function khz(hz) { return hz < 1e6 ? String(+(hz / 1000).toFixed(2)) + ' kHz' : mhz(hz) + ' MHz'; }
// The network details' merge section: what it is made of (with Unmerge) and
// a picker to merge it into another network.
function mergeSection(d, n) {
  var parts = (n.parts || [n]).filter(function (p) { return p.key !== n.key; });
  if (parts.length) {
    d.appendChild(h('h4', { text: 'Merged networks' }));
    d.appendChild(h('div', { class: 'hint', style: 'font-size:.8rem;margin-bottom:.3rem',
      text: 'Shown as one network' + (S.file ? ' in this file view.' : ' for everyone viewing this server, and in exports.') +
            ' The networks stay apart underneath: Unmerge separates them again.' }));
    var ul = h('div', { class: 'mlist' });
    (n.parts || []).forEach(function (p) {
      var main = p.key === n.key;
      ul.appendChild(h('div', { class: 'mrow' }, [
        h('span', { class: 'ml' }, [h('b', { text: p.label }), h('span', { class: 'alias', text: ' · ' + p.calls + ' calls' +
          ((p.freqs || []).some(function (f) { return p.label.indexOf(mhz(f)) < 0; }) ? ' · ' + p.freqs.map(mhz).join(', ') + ' MHz' : '') +
          (main ? ' · main' : '') })]),
        main ? null : h('button', { class: 'btn sm', type: 'button', title: 'Show it as a network of its own again',
                                    onclick: function () { unmerge([p.key]); } }, 'Unmerge')]));
    });
    d.appendChild(ul);
    if (parts.length > 1)
      d.appendChild(h('button', { class: 'btn sm', type: 'button', style: 'margin-top:.3rem', onclick: function () { unmerge([n.key]); } }, 'Unmerge all'));
  }
  var cands = mergeCandidates(n);
  if (!cands.length) return;
  var selEl = h('select', { class: 'msel', 'aria-label': 'Network to merge this one into', 'data-net': n.key },
    [h('option', { value: '' }, 'Merge into…')].concat(cands.map(function (c) {
      var why = [];
      if (c.j) why.push(TIER[c.j.tier]);
      if (c.j && c.j.twins) why.push(c.j.twins + ' on both');
      if (c.same) why.push('same code');
      if (isFinite(c.d)) why.push(c.d ? khz(c.d) + ' away' : 'same channel');
      if (c.s) why.push(c.s + ' shared');
      return h('option', { value: c.n.key }, c.n.label + (why.length ? '  (' + why.join(', ') + ')' : ''));
    })));
  var go = h('button', { class: 'btn sm', type: 'button', disabled: true,
    title: 'Show this network as part of the one picked (one channel heard at two frequency offsets, say)',
    onclick: function () { if (selEl.value) mergeInto([n.key], selEl.value); } }, 'Merge');
  // The pick survives the panel being rebuilt (S.mpick: this network's).
  if (S.mpick && S.mpick.net === n.key && cands.some(function (c) { return c.n.key === S.mpick.to; })) {
    selEl.value = S.mpick.to;
    go.disabled = false;
  }
  selEl.addEventListener('change', function () {
    go.disabled = !selEl.value;
    S.mpick = selEl.value ? { net: n.key, to: selEl.value } : null;
  });
  // Rebuilds held off while it was in use (see mergePickBusy) catch up.
  selEl.addEventListener('blur', function () { setTimeout(function () { if (!mergePickBusy()) renderDetail(); }, 0); });
  d.appendChild(h('div', { class: 'tagrow mpick' }, [selEl, go]));
}
// The Merge into… picker is in use: its list is open (it has focus) or the
// pointer is on it (about to click Merge). Rebuilding the details now would
// close the list or swallow the click, so renderDetail waits.
function mergePickBusy() {
  var ae = document.activeElement, sel = S.sel;
  if (!sel || sel.type !== 'net') return false;
  var row = $('detail').querySelector('.mpick');
  if (!row) return false;
  if (ae && ae.classList && ae.classList.contains('msel') && row.contains(ae) && ae.getAttribute('data-net') === sel.id) return true;
  try { return row.matches(':hover'); } catch (e) { return false; }
}

// ---------- indexing ----------
function colorFor(key) {
  var n = IX && IX.netByKey[key];
  if (!n || n.confidence === 'none') return '#7a8288';
  var ck = S.fam + '|' + key;
  if (!S.ncol[ck]) { var i = S.nidx[S.fam] || 0; S.ncol[ck] = PAL[i % PAL.length]; S.nidx[S.fam] = i + 1; }
  return S.ncol[ck];
}
// A network to list: identified (any code / system), or unidentified but with
// traffic (a call was heard on it). A bare "Unidentified · <freq>" with nothing
// decoded yet is hidden -- a receiver is tuned there but there is nothing to
// show, and it would come and go as streams start and stop.
function netShown(n) { return !unidentifiedNet(n) || n.calls > 0; }
function index() {
  var F = S.d.families[S.fam];
  var X = { F: F, nets: F.networks.filter(netShown), netByKey: {}, tgs: F.talkgroups, tgById: {}, radios: F.radios,
            rById: {}, calls: F.calls, netTg: {}, netRad: {} };
  X.nets.sort(function (a, b) { return a.first - b.first; });
  X.nets.forEach(function (n) { X.netByKey[n.key] = n; });
  IX = X;
  X.nets.forEach(function (n) { colorFor(n.key); });
  X.tgs.forEach(function (t) { X.tgById[t.id] = t; t.networks.forEach(function (k) { X.netTg[k] = (X.netTg[k] || 0) + 1; }); });
  X.radios.forEach(function (r) { X.rById[r.id] = r; r.networks.forEach(function (k) { X.netRad[k] = (X.netRad[k] || 0) + 1; }); });
  // Order networks by calls so a node's "primary" network is its busiest one.
  X.netRank = {};
  X.nets.slice().sort(function (a, b) { return b.calls - a.calls; }).forEach(function (n, i) { X.netRank[n.key] = i; });
}
// A node on several networks is drawn in one network's colour: the busiest
// when it is first drawn, then kept (NODENET) for as long as it is still on
// that network. Re-picking the busiest on every update made shared nodes
// flip colour back and forth whenever two networks' call totals crossed.
var NODENET = {};
function primaryNet(list, id) {
  list = list || [];
  var key = id ? S.fam + '|' + id : null, kept = key && NODENET[key];
  if (kept && list.indexOf(kept) >= 0) return kept;
  var best = null;
  list.forEach(function (k) { if (best == null || (IX.netRank[k] || 0) < (IX.netRank[best] || 0)) best = k; });
  if (key && best) NODENET[key] = best;
  return best;
}
// The network filter (S.nets): key -> 'in' (picked: show only the picked
// ones) or 'out' (excluded). Empty = every network.
function netAll() { for (var k in S.nets) return false; return true; }
function netKeys() { return Object.keys(S.nets); }
function netOk(k) {
  if (S.nets[k] === 'out') return false;
  if (S.nets[k] === 'in') return true;
  for (var x in S.nets) if (S.nets[x] === 'in') return false;
  return true;
}
function inNet(list) { return netAll() || (list || []).some(netOk); }
// The talkgroup (S.tgf) and radio (S.rf) filters, the same way: id -> 'in'
// (only the picked ones) or 'out' (excluded). Empty = all of them.
function fAll(m) { for (var k in m) return false; return true; }
function fPicked(m) { for (var k in m) if (m[k] === 'in') return true; return false; }
function fOk(m, id) { return m[id] === 'in' || (m[id] !== 'out' && !fPicked(m)); }
function tgAll() { return fAll(S.tgf); }
function tgPicked() { return fPicked(S.tgf); }
function tgOk(id) { return fOk(S.tgf, id); }
// A call passes the radio filter unless a radio at either end is excluded;
// with radios picked, one of them must be at an end.
function callROk(c) {
  if (fAll(S.rf)) return true;
  var t = c.priv ? c.tgt : null;
  if (S.rf[c.src] === 'out' || (t && S.rf[t] === 'out')) return false;
  return !fPicked(S.rf) || S.rf[c.src] === 'in' || !!(t && S.rf[t] === 'in');
}
// With radios picked, a talkgroup passes if one of them used it.
function tgROk(t) { return !fPicked(S.rf) || keys(t.radios).some(function (r) { return S.rf[r] === 'in'; }); }
// A group call passes with its talkgroup; a private call only while no
// talkgroup is picked (it has none).
function callTgOk(c) { return tgAll() || (c.priv ? !tgPicked() : tgOk(c.tgt)); }
// A radio passes if it used a talkgroup that passes -- or, with only
// exclusions, if it used no talkgroup or made private calls.
function radioTgOk(r) {
  if (tgAll()) return true;
  var ts = keys(r.tgs);
  if (ts.some(tgOk)) return true;
  return !tgPicked() && (!ts.length || keys(r.peers).length > 0);
}
function anyFilter() { return !netAll() || !tgAll() || !fAll(S.rf); }
function qm() {
  var q = S.q.toLowerCase();
  for (var i = 0; i < arguments.length; i++) {
    var v = arguments[i];
    if (v == null) continue;
    if (Array.isArray(v)) { for (var j = 0; j < v.length; j++) if (String(v[j]).toLowerCase().indexOf(q) >= 0) return true; }
    else if (String(v).toLowerCase().indexOf(q) >= 0) return true;
  }
  return false;
}
// The calls the filters let through. `allAudio`: ignore "With audio only" (a
// Calls-list option; the stat cards count every call).
function fCalls(list, allAudio) {
  var aud = S.audOnly && (!S.file || !fAll(FILEAUDIO)) && !allAudio;
  return (list || IX.calls).filter(function (c) {
    return (netAll() || netOk(c.net)) && callTgOk(c) && callROk(c) && (!aud || hasAudio(c)) &&
      (!S.q || qm(c.src, c.tgt, c.alias, c.text, svcLabel(c.svc), mhz(c.freq), IX.rById[c.src] && IX.rById[c.src].aliases,
                   c.kid ? ['0x' + c.kid, algName(c.alg)] : null));
  });
}
function fTgs() { return IX.tgs.filter(function (t) { return inNet(t.networks) && tgOk(t.id) && tgROk(t) && (!S.q || qm(t.id)); }); }
function fRadios() { return IX.radios.filter(function (r) { return inNet(r.networks) && radioTgOk(r) && fOk(S.rf, r.id) && (!S.q || qm(r.id, r.aliases)); }); }

// ---------- entity widgets ----------
function aliasOf(id) { var r = IX.rById[id]; return r && r.aliases.length ? r.aliases[r.aliases.length - 1] : ''; }
function rlink(id, alias) {
  if (alias == null) alias = aliasOf(id);
  return h('a', { class: 'ent', href: '#', title: 'Radio ' + id,
                  onclick: function (e) { e.preventDefault(); e.stopPropagation(); select('radio', id); } },
           [id, alias ? h('span', { class: 'alias', text: ' ' + alias }) : null]);
}
function tlink(id) {
  return h('a', { class: 'ent', href: '#', title: 'Talkgroup ' + id,
                  onclick: function (e) { e.preventDefault(); e.stopPropagation(); select('tg', id); } }, ['TG ', id]);
}
function sw(key) { return h('span', { class: 'sw', style: 'background:' + colorFor(key) }); }
// `hz`: a frequency shown next to it already -- left out of the label
// ("Color Code 5 · 460.0250 MHz" -> "Color Code 5"); the tooltip keeps it.
function netc(key, hz) {
  var n = IX.netByKey[key], label = n ? n.label : key;
  var short = hz ? label.replace(' \u00B7 ' + mhz(hz) + ' MHz', '') : label;
  // A merged network spans channels: its own label's one goes too.
  if (hz && n && n.parts && n.parts.length > 1) short = short.replace(/ \u00B7 [\d.]+ MHz$/, '');
  return h('span', { class: 'netc', title: label,
                     onclick: function (e) { e.stopPropagation(); select('net', key); } }, [sw(key), short || label]);
}
function nets(list) {
  var w = h('span');
  (list || []).forEach(function (k, i) { if (i) w.appendChild(document.createTextNode(' ')); w.appendChild(netc(k)); });
  return w;
}
function badge(cls, t) { return h('span', { class: 'badge ' + cls, text: t }); }
// 434425000 -> "434.4250" (MHz; more decimals only when needed)
function mhz(hz) {
  if (!hz) return '';
  var t = (hz / 1e6).toFixed(6);
  while (t.length > t.indexOf('.') + 5 && t.charAt(t.length - 1) === '0') t = t.slice(0, -1);
  return t;
}
var CONF_TIP = {
  strong: 'Strong identity: a system id (P25 WACN/SysID, DMR network id, NXDN system code, TETRA MCC/MNC).',
  channel: 'Channel identity: a short code (or nothing) on a known frequency -- one conventional channel.',
  weak: 'Weak identity: only a short code on an unknown frequency; covers one stream.',
  none: 'Nothing identifying decoded yet.' };
function confBadge(c) {
  var b = badge('b-' + c, c === 'none' ? 'unidentified' : c);
  if (CONF_TIP[c]) { b.title = CONF_TIP[c]; b.setAttribute('data-tip', CONF_TIP[c]); }
  return b;
}

// ---------- layout helpers ----------
function mq(q) { return window.matchMedia ? window.matchMedia(q).matches : false; }
function phone() { return mq('(max-width: 640px)'); }
function drawer() { return mq('(max-width: 1050px)'); }
function coarse() { return mq('(pointer: coarse)'); }
var toastT = 0;
function toast(text) {
  var t = $('toast');
  if (!t) { t = h('div', { id: 'toast', class: 'toast', role: 'status' }); document.body.appendChild(t); }
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toastT);
  toastT = setTimeout(function () { t.hidden = true; }, 3500);
}
// Hover-only explanations (title=...) are shown on tap on touch screens.
document.addEventListener('click', function (e) {
  var el = e.target.closest && e.target.closest('[data-tip]');
  if (el && coarse()) toast(el.getAttribute('data-tip'));
});

// ---------- generic sortable table ----------
// On phones it renders as stacked cards (CSS) with a sort menu in place of
// the column headers; `card`, if given, renders each row as a custom card.
function table(cont, view, cols, rows, empty, onRow, selFn, card, sub) {
  // Narrow screens drop columns that are empty for every row (dropEmpty: the
  // test for a value) or only repeat what the details show (hideMd).
  cols.forEach(function (c, i) { c.i = i; });
  var all = cols;
  cols = cols.filter(function (c) { return !c.hideEmpty || rows.some(c.hideEmpty); });
  if (drawer())
    cols = cols.filter(function (c) {
      if (c.hideMd && mq('(max-width: 900px)')) return false;
      return !c.dropEmpty || rows.some(c.dropEmpty);
    });
  var st = S.sort[view];
  if (st && all[st.i] && all[st.i].k) {
    var kf = all[st.i].k;
    rows = rows.slice().sort(function (a, b) {
      var x = kf(a), y = kf(b);
      if (typeof x === 'string' && typeof y === 'string') return x.localeCompare(y, undefined, { numeric: true }) * st.dir;
      return (x < y ? -1 : x > y ? 1 : 0) * st.dir;
    });
  }
  S.rows[view] = rows;                                  // as listed (the audio .zip follows it)
  var head = h('tr', null, cols.map(function (c) {
    var i = c.i, arrow = st && st.i === i ? (st.dir > 0 ? ' ▲' : ' ▼') : '';
    return h('th', { class: (/\bnum\b/.test(c.cls || '') ? 'num' : '') + (c.k ? ' sortable' : ''),
                     onclick: c.k ? function () {
                       var cur = S.sort[view];
                       S.sort[view] = cur && cur.i === i ? { i: i, dir: -cur.dir } : { i: i, dir: c.dir || -1 };
                       renderView();
                     } : null }, c.label + arrow);
  }));
  var y = cont.scrollTop;
  cont.textContent = '';
  if (phone()) {
    var sb = h('select', { 'aria-label': 'Sort by', onchange: function () {
      var v = this.value.split(':');
      S.sort[view] = v[0] === '' ? null : { i: +v[0], dir: +v[1] };
      renderView();
    } }, [h('option', { value: ':' }, 'Default order')]);
    cols.forEach(function (c) {
      var i = c.i;
      if (!c.k) return;
      [[c.dir || -1, ''], [-(c.dir || -1), '']].forEach(function (d) {
        var o = h('option', { value: i + ':' + d[0] }, c.label + (d[0] > 0 ? ' \u25B2' : ' \u25BC'));
        if (st && st.i === i && st.dir === d[0]) o.selected = true;
        sb.appendChild(o);
      });
    });
    cont.appendChild(h('div', { class: 'sortbar' }, [h('span', null, 'Sort'), sb,
      h('span', { text: rows.length + (rows.length === 1 ? ' row' : ' rows') })]));
  }
  if (card && phone()) {
    var list = h('div', { class: 'clist' });
    if (!rows.length) list.appendChild(h('div', { class: 'empty', text: empty }));
    rows.slice(0, ROWS).forEach(function (r) { list.appendChild(card(r)); });
    cont.appendChild(list);
  } else {
    var tb = h('tbody');
    if (!rows.length) tb.appendChild(h('tr', null, h('td', { class: 'empty', colspan: cols.length, text: empty })));
    rows.slice(0, ROWS).forEach(function (r, i) {
      var below = sub && sub(r), alt = i % 2 ? ' alt' : '';
      tb.appendChild(h('tr', { class: (onRow ? 'click' : '') + (selFn && selFn(r) ? ' sel' : '') + alt + (below ? ' hassub' : ''),
                               onclick: onRow ? function () { onRow(r); } : null },
        cols.map(function (c) { return h('td', { class: c.cls || '', 'data-label': c.label }, c.cell(r)); })));
      if (below) tb.appendChild(h('tr', { class: 'sub' + alt }, h('td', { colspan: cols.length }, below)));
    });
    cont.appendChild(h('table', null, [h('thead', null, head), tb]));
  }
  if (rows.length > ROWS) cont.appendChild(h('div', { class: 'more', text: 'Showing ' + ROWS + ' of ' + rows.length +
                                                      ' — narrow it with the search box or a network filter.' }));
  cont.scrollTop = y;
}
function isSel(type, id) { return S.sel && S.sel.type === type && S.sel.id === id; }

// ---------- views ----------
function durCell(c) {
  return live(c) ? h('span', { class: 'livecell' }, [h('span', { class: 'dot' }), dur(now() - c.start)])
                 : dur(c.last - c.start);
}
function toCell(c) { return !c.tgt ? '—' : c.priv ? h('span', null, ['⇄ ', rlink(c.tgt)]) : tlink(c.tgt); }
// Encryption: the algorithm and key id an encrypted call named (hex, as the
// protocol numbers them; the server counts each "alg:kid" per network,
// talkgroup and radio). Names per protocol, as dsd-fme reports them.
var ENCALG = {
  p25: { '80': 'clear', '81': 'DES-OFB', '82': '2-key 3DES', '83': '3DES', '84': 'AES-256', '85': 'AES-128',
         '88': 'AES-CBC', '89': 'AES-128-OFB', '9F': 'DES-XL', 'A0': 'DVI-XL', 'A1': 'DVP-XL', 'AA': 'ADP (RC4)' },
  dmr: { '21': 'RC4 (EP)', '22': 'DES', '24': 'AES-128', '25': 'AES-256' },
  nxdn: { '01': 'Scrambler', '02': 'DES', '03': 'AES' }
};
function algName(alg, fam) {
  var t = ENCALG[fam || S.fam];
  return !alg ? 'Unknown algorithm' : (t && t[alg]) || 'ALG 0x' + alg;
}
function keyText(alg, kid, fam) { return algName(alg, fam) + ' \u00B7 key 0x' + kid; }
// A "Keys seen" section in a network / talkgroup / radio's details.
// How many hex digits a key for this algorithm is expected to have (a hint
// for the input; the server validates). 0 = unknown, any even hex up to 64.
function keyLen(alg) {
  var p = ENCALG[S.fam] || {};
  if (p === ENCALG.p25) return alg === '84' ? 64 : alg === '85' || alg === '89' ? 32 : alg === 'AA' || alg === '81' ? 16 : 0;
  if (p === ENCALG.dmr) return alg === '25' ? 64 : alg === '24' ? 32 : alg === '21' ? 10 : alg === '22' ? 16 : 0;
  return 0;
}
// Is a decryption key loaded (on the server) for this key id on any of these
// networks? S.d.keyed is {fam:{net:[kid,…]}} -- ids only, never values.
function keyedHas(nets, kid) {
  var K = S.d && S.d.keyed && S.d.keyed[S.fam];
  if (!K) return false;
  var id = (kid || '').toUpperCase();
  for (var i = 0; i < (nets || []).length; i++) { var a = K[nets[i]]; if (a && a.indexOf(id) >= 0) return true; }
  return false;
}
function markKeyed(net, kid, on) {                       // reflect a set/remove at once (poll confirms)
  if (!S.d) return;
  S.d.keyed = S.d.keyed || {};
  var F = S.d.keyed[S.fam] = S.d.keyed[S.fam] || {}, a = F[net] = F[net] || [], id = (kid || '').toUpperCase(), i = a.indexOf(id);
  if (on && i < 0) a.push(id); else if (!on && i >= 0) a.splice(i, 1);
}
function setKey(net, kid, alg, value, done) {
  fetch('/net/keys/set', { method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
                           body: JSON.stringify({ fam: S.fam, net: net, kid: kid, alg: alg, key: value }) })
    .then(function (r) { return r.json().then(function (j) { return r.ok && j.ok; }, function () { return false; }); })
    .then(function (ok) {
      if (ok) { markKeyed(net, kid, true); toast('Key loaded for ' + keyText(alg, kid) + '.'); renderDetail(true); }
      else toast('That key wasn’t accepted (expected hex digits, up to 64).');
      if (done) done(ok);
    }).catch(function () { toast('Could not reach the server to set the key.'); });
}
function removeKey(net, kid) {
  fetch('/net/keys/remove?fam=' + encodeURIComponent(S.fam) + '&net=' + encodeURIComponent(net) + '&kid=' + encodeURIComponent(kid),
        { cache: 'no-store' })
    .then(function (r) { return r.json(); }).then(function (j) {
      if (j.ok) { markKeyed(net, kid, false); toast('Key removed.'); renderDetail(true); }
    }).catch(function () { toast('Could not reach the server to remove the key.'); });
}
// "Encryption keys seen": each key id the entity's encrypted calls named, how
// many calls, and whether a decryption key is loaded. On a network's panel
// (live view) a key can be added / removed and the key list downloaded.
function keysSection(d, m, what, opts) {
  opts = opts || {};
  var ks = keys(m || {});
  if (!ks.length) return;
  var nets = opts.nets || [], editNet = opts.net && !S.file ? opts.net : null;
  d.appendChild(h('h4', { text: 'Encryption keys seen' }));
  var ul = h('ul', { class: 'lst keylst' });
  ks.sort(function (a, b) { return m[b] - m[a]; }).forEach(function (k) {
    var i = k.indexOf(':'), alg = k.slice(0, i), kid = k.slice(i + 1), loaded = keyedHas(nets, kid);
    var row = h('li', null, [h('div', { class: 'keyhdr' }, [
      h('span', { class: 'mono', text: keyText(alg, kid) }),
      loaded ? h('span', { class: 'keyok', text: '✓ key loaded' }) : null,
      h('span', { class: 'c', text: m[k] + ' call' + (m[k] > 1 ? 's' : '') })])]);
    if (editNet) {
      var act = h('div', { class: 'keyact' });
      var showForm = function () {
        act.textContent = '';
        var want = keyLen(alg);
        var inp = h('input', { class: 'ki', type: 'text', spellcheck: 'false', autocomplete: 'off',
                               placeholder: want ? want + ' hex digits' : 'key (hex)', 'aria-label': 'Key for ' + keyText(alg, kid) });
        var save = function () { if (inp.value.trim()) setKey(editNet, kid, alg, inp.value.trim()); };
        inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') save(); if (e.key === 'Escape') showButtons(); });
        act.appendChild(inp);
        act.appendChild(h('button', { class: 'btn sm', type: 'button', onclick: save }, 'Save'));
        act.appendChild(h('button', { class: 'btn sm', type: 'button', onclick: showButtons }, 'Cancel'));
        act.appendChild(h('span', { class: 'kh', text: 'Hex, up to 64 digits' + (want ? ' (' + algName(alg) + ' uses ' + want + ')' : '') + '. Stored on the server; never shown again.' }));
        inp.focus();
      };
      var showButtons = function () {
        act.textContent = '';
        act.appendChild(h('button', { class: 'btn sm', type: 'button', onclick: showForm }, loaded ? 'Replace key' : 'Add key…'));
        if (loaded) act.appendChild(h('a', { class: 'keyrm', onclick: function () { removeKey(editNet, kid); } }, 'Remove'));
      };
      showButtons();
      row.appendChild(act);
    }
    ul.appendChild(row);
  });
  d.appendChild(ul);
  if (editNet && nets.some(function (n) { return (S.d.keyed && S.d.keyed[S.fam] && S.d.keyed[S.fam][n] || []).length; }))
    d.appendChild(h('a', { class: 'btn sm', style: 'margin-top:.4rem', href: '/net/keys/list?fam=' + encodeURIComponent(S.fam) +
                           '&net=' + encodeURIComponent(editNet), download: 'dsd_keys_' + S.fam + '.csv',
                          title: 'Download this network’s keys as a dsd-fme key list (feed your decoder with -K)' }, '⤓ Download key list'));
  d.appendChild(h('div', { class: 'hint', style: 'margin-top:.4rem;font-size:.8rem',
    text: 'The key id each encrypted call ' + what + ' announced. A key belongs to the talkgroup or channel, not the radio: every radio talking on a talkgroup uses its key.' +
          (editNet ? ' Keys you add are kept on the server and used by your own decoder via the downloaded key list — the explorer does not decrypt. Only enter keys for systems you are authorized to monitor.' : '') }));
}
function typeBadges(c) {
  var rx = 'Heard by ' + c.streams + ' receivers (one call, deduplicated)';
  var et = c.kid ? 'Encrypted: ' + keyText(c.alg, c.kid) : 'Encrypted (its key id wasn\u2019t decoded)';
  return h('span', null, [
    (c.data && !c.voice) ? badge('b-data', 'DATA') : badge('b-voice', 'VOICE'),
    c.priv ? badge('b-priv', 'PRIVATE') : badge('b-group', 'GROUP'),
    c.emerg ? badge('b-emerg', 'EMERGENCY') : null, c.enc ? h('span', { class: 'badge b-enc', title: et, 'data-tip': et }, 'ENCRYPTED') : null,
    c.streams > 1 ? h('span', { class: 'badge b-group', title: rx, 'data-tip': rx }, c.streams + ' RX') : null]);
}
// ---------- call audio ----------
var PLAYER = { a: null, name: null, call: null };
function mmss(ms) { var s = Math.round(ms / 1000); return Math.floor(s / 60) + ':' + p2(s % 60); }
// A call's identity across polls (its id can change as imports come and go).
// In a file view, a call's audio comes from the opened "export with audio"
// zip (FILEAUDIO: audio name -> blob URL); live, from the server.
var FILEAUDIO = {};
function clearFileAudio() { for (var k in FILEAUDIO) URL.revokeObjectURL(FILEAUDIO[k]); FILEAUDIO = {}; }
function hasAudio(c) { return !!c.audio && (!S.file || !!FILEAUDIO[c.audio]); }
function audioUrl(c) { return S.file ? (FILEAUDIO[c.audio] || '') : '/net/audio/' + encodeURIComponent(c.audio); }
function stamp(ms) { return dt(ms).replace(/[-:]/g, '').replace(' ', 'T'); }
function safeName(s) { return String(s).replace(/[^A-Za-z0-9.-]+/g, '_'); }
// A descriptive name for a call's audio file: when, where, to and from whom.
function callFile(c) {
  return 'call_' + stamp(c.start) + (c.freq ? '_' + mhz(c.freq) + 'MHz' : '') + (c.slot ? '_s' + c.slot : '') +
         '_' + (c.priv ? 'to_' : 'TG') + safeName(c.tgt || 'x') + '_from_' + safeName(c.src || 'x') + '.wav';
}
function netLabel(k) { return k ? (IX && IX.netByKey[k] ? IX.netByKey[k].label : k) : ''; }
function syncPlay() {
  document.querySelectorAll('.play[data-audio]').forEach(function (b) {
    var on = b.getAttribute('data-audio') === PLAYER.name;
    b.classList.toggle('on', on);
    b.firstChild.textContent = on ? '■' : '▶';
  });
  var np = $('npplay');
  np.classList.toggle('on', !!PLAYER.name);
  np.firstChild.textContent = PLAYER.name ? '■' : '▶';
}
function playAudio(c) {
  if (!PLAYER.a) {
    PLAYER.a = new Audio();
    PLAYER.a.addEventListener('ended', function () { if (!followLive()) playDone(); });
    PLAYER.a.addEventListener('error', function () { if (PLAYER.name) toast('This call\'s audio is no longer available.'); PLAYER.name = null; syncPlay(); });
    PLAYER.a.addEventListener('timeupdate', npProgress);
  }
  if (PLAYER.name === c.audio) { PLAYER.a.pause(); PLAYER.name = null; syncPlay(); return; }
  PLAYER.name = c.audio;
  PLAYER.call = c;
  PLAYER.waits = 0;
  PLAYER.a.src = audioUrl(c);
  var p = PLAYER.a.play();
  if (p && p.catch) p.catch(function () {});
  syncPlay();
  showNp();
  // A live call is transcribed once it is over (its audio is still growing).
  PLAYER.txLater = ASR.on && live(c);
  if (ASR.on && !PLAYER.txLater) transcribe(c);
}
// This call's latest record (the list is rebuilt on every poll).
function callNow(c) { return (IX && IX.calls.filter(function (x) { return x.audio === c.audio; })[0]) || c; }
function playDone() {
  var c = PLAYER.call;
  PLAYER.name = null; syncPlay(); npProgress();
  if (PLAYER.txLater && c) { PLAYER.txLater = false; transcribe(callNow(c)); }
}
// A live call's audio file is still being written: the server sends it as far
// as it has got. At the end of that, fetch it again and carry on from the
// same point -- waiting a moment when nothing new has arrived yet -- until
// the call is over and all of it has played.
function followLive() {
  var c = PLAYER.call, a = PLAYER.a;
  if (!c || !PLAYER.name || S.file) return false;
  var cur = callNow(c), at = a.duration || a.currentTime || 0;
  if (!(live(cur) || (cur.audio_ms || 0) / 1000 > at + 0.15) || ++PLAYER.waits > 120) return false;
  var name = PLAYER.name;
  a.addEventListener('loadedmetadata', function once() {
    a.removeEventListener('loadedmetadata', once);
    if (PLAYER.name !== name) return;
    if (a.duration > at + 0.05) {                    // more audio: go on from where it stopped
      PLAYER.waits = 0;
      a.currentTime = at;
      var p = a.play();
      if (p && p.catch) p.catch(function () {});
    } else setTimeout(function () { if (PLAYER.name === name && !followLive()) playDone(); }, 500);
  });
  a.src = audioUrl(cur) + '?t=' + Date.now();
  return true;
}
function playBtn(c) {
  if (!hasAudio(c)) return null;
  return h('button', { class: 'play' + (PLAYER.name === c.audio ? ' on' : ''), type: 'button', 'data-audio': c.audio,
                       title: 'Play this call\'s audio' + (c.audio_ms ? ' (' + mmss(c.audio_ms) + ')' : '') +
                              (ASR.on ? ' and transcribe it' : ''),
                       onclick: function (e) { e.stopPropagation(); playAudio(c); } },
           [h('span', { text: PLAYER.name === c.audio ? '■' : '▶' }), c.audio_ms ? mmss(c.audio_ms) : '']);
}
function dlLink(c) {
  return h('a', { class: 'dl', href: audioUrl(c), download: callFile(c), title: 'Download this call\'s audio (.wav)',
                  onclick: function (e) { e.stopPropagation(); } }, '⤓');
}
function audioCell(c) { var p = playBtn(c); return p ? h('span', { class: 'nowrap' }, [p, dlLink(c)]) : ''; }
// A data call's service, in words (svc from the server: what its header /
// Motorola MNIS service said).
var SVC = { preamble: 'Data announced only', ack: 'ACK (delivery confirmed)', data: 'Data packet',
            ars: 'ARS (registration)', lrrp: 'LRRP (location)', tms: 'TMS (text message)' };
function svcLabel(s) {
  if (!s) return '';
  if (SVC[s]) return SVC[s];
  var m = /^mnis:(\w+)$/.exec(s);
  return m ? 'Moto data (service 0x' + m[1] + ')' : s.toUpperCase();
}
function svcTip(s) {
  return s === 'preamble' ? 'Only the announcement of a data transfer was heard here (it may have gone out on another channel)' :
         s === 'ack' ? 'The receiving radio (or gateway) confirming it got a data packet' :
         s === 'ars' ? 'Motorola Automatic Registration Service: a radio registering with (or being polled by) the data gateway' :
         /^mnis:/.test(s) ? 'A Motorola (MNIS) data packet of a service with no published format \u2014 0x80 and 0x20 are common, radio to data gateway; its contents are binary and not decoded' : null;
}
// A position report: "lat, lon" linked to a map.
function posLink(pos) {
  var p = (pos || '').split(','), la = p[0], lo = p[1];
  return h('a', { class: 'pos', href: 'https://www.google.com/maps/search/?api=1&query=' + la + ',' + lo,
                  target: '_blank', rel: 'noopener', title: 'Position report \u2014 open in Google Maps',
                  onclick: function (e) { e.stopPropagation(); } },
           [h('span', { class: 'nw', text: '\u{1F4CD} ' + la + ',' }), ' ', h('span', { class: 'nw', text: lo })]);
}
// ---- maps: tiles from a public (or your own) tile server, drawn by the
// page -- no key, no library. Positions, paths and labels are an SVG layer
// on top. A single position also links to Google Maps (posLink). ----
var TILESETS = {
  osm:  { name: 'OpenStreetMap', url: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png', max: 19,
          attrib: '\u00A9 OpenStreetMap contributors' },
  dark: { name: 'CARTO Dark', url: 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png', sub: 'abcd', max: 19,
          attrib: '\u00A9 OpenStreetMap contributors \u00A9 CARTO' },
  sat:  { name: 'Esri imagery', url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', max: 19,
          attrib: 'Imagery \u00A9 Esri, Maxar, Earthstar Geographics' },
  topo: { name: 'OpenTopoMap', url: 'https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', sub: 'abc', max: 17,
          attrib: '\u00A9 OpenStreetMap contributors, SRTM \u00B7 \u00A9 OpenTopoMap (CC-BY-SA)' },
  off:  { name: 'No map (sketch only)' }
};
// The tile sets on offer: the server's own (DSD_NET_MAP_TILES) first, if set.
function tilesets() {
  var m = S.d && S.d.map, out = {};
  if (m && m.tiles) out.server = { name: 'This server\u2019s map', url: m.tiles, max: 19, attrib: m.attrib || '' };
  for (var k in TILESETS) out[k] = TILESETS[k];
  return out;
}
function tileKey() { var t = tilesets(), k = load('map.tiles'); return t[k] ? k : t.server ? 'server' : 'osm'; }
function tileset() { return tilesets()[tileKey()]; }
function wx(lon, z) { return (lon + 180) / 360 * 256 * Math.pow(2, z); }
function wy(lat, z) {
  var s = Math.sin(Math.max(-85.05, Math.min(85.05, lat)) * Math.PI / 180);
  return (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * 256 * Math.pow(2, z);
}
// A slippy map. set(layers, key) gives it lines (paths) and points; a new
// key fits the view to them, the same key keeps the user's pan / zoom (the
// page re-renders on every poll). Tiles already loaded are reused.
function TileMap(opts) {
  var m = this;
  m.opts = opts || {};
  m.z = 2; m.cx = 512; m.cy = 512;           // the world, centred (until fit())
  m.key = null; m.layers = { lines: [], pts: [] }; m.imgs = {}; m.ok = 0; m.bad = 0; m.ts = null; m.hover = null;
  m.tiles = h('div', { class: 'tm-tiles' });
  m.svg = sv('svg', { class: 'tm-ov' });
  m.att = h('div', { class: 'tm-att' });
  m.msg = h('div', { class: 'tm-msg', hidden: true });
  var zb = function (t, title, f) { return h('button', { class: 'btn', type: 'button', title: title, 'aria-label': title,
                                                         onclick: function (e) { e.stopPropagation(); f(); } }, t); };
  m.el = h('div', { class: 'tmap' }, [m.tiles, m.svg,
    h('div', { class: 'tm-zoom' }, [zb('+', 'Zoom in', function () { m.zoomAt(1); }), zb('\u2212', 'Zoom out', function () { m.zoomAt(-1); }),
                                    zb('\u2922', 'Fit', function () { m.fit(); m.draw(); })]), m.att, m.msg]);
  var drag = null;
  m.el.addEventListener('pointerdown', function (e) {
    if (e.button !== 0 || e.target.closest('.tm-zoom') || e.target.closest('.tm-pt') || e.target.closest('.tm-hit')) return;
    drag = { x: e.clientX, y: e.clientY, cx: m.cx, cy: m.cy };
    m.el.setPointerCapture(e.pointerId); m.el.classList.add('drag');
  });
  m.el.addEventListener('pointermove', function (e) {
    if (!drag) return;
    m.cx = drag.cx - (e.clientX - drag.x); m.cy = drag.cy - (e.clientY - drag.y); m.draw();
  });
  var end = function () { drag = null; m.el.classList.remove('drag'); };
  // Where the pointer is (for the hover highlight: a redraw replaces the
  // element under it without a mouseleave, so draw() looks again).
  m.el.addEventListener('pointermove', function (e) { m.px = e.clientX; m.py = e.clientY; });
  m.el.addEventListener('pointerleave', function () { m.px = null; if (m.hover != null) { m.hover = null; m.hl(); } });
  m.el.addEventListener('pointerup', end); m.el.addEventListener('pointercancel', end);
  m.el.addEventListener('wheel', function (e) {
    e.preventDefault();
    var r = m.el.getBoundingClientRect();
    m.zoomAt(e.deltaY < 0 ? 1 : -1, e.clientX - r.left - r.width / 2, e.clientY - r.top - r.height / 2);
  }, { passive: false });
  m.el.addEventListener('dblclick', function (e) {
    if (e.target.closest('.tm-zoom')) return;
    var r = m.el.getBoundingClientRect();
    m.zoomAt(1, e.clientX - r.left - r.width / 2, e.clientY - r.top - r.height / 2);
  });
}
TileMap.prototype.maxZ = function () { return (this.ts && this.ts.max) || 19; };
TileMap.prototype.zoomAt = function (dz, ox, oy) {
  var z = Math.max(1, Math.min(this.maxZ(), this.z + dz));
  if (z === this.z) return;
  var f = Math.pow(2, z - this.z);
  ox = ox || 0; oy = oy || 0;
  this.cx = (this.cx + ox) * f - ox; this.cy = (this.cy + oy) * f - oy; this.z = z;
  this.draw();
};
TileMap.prototype.bounds = function () {
  var b = null, add = function (p) {
    if (!b) b = [p[0], p[1], p[0], p[1]];
    else { b[0] = Math.min(b[0], p[0]); b[1] = Math.min(b[1], p[1]); b[2] = Math.max(b[2], p[0]); b[3] = Math.max(b[3], p[1]); }
  };
  this.layers.lines.forEach(function (l) { l.pts.forEach(add); });
  this.layers.pts.forEach(function (p) { add(p.ll); });
  return b;
};
TileMap.prototype.fit = function () {
  var b = this.bounds(), w = this.el.clientWidth || 300, hh = this.el.clientHeight || 200;
  this.empty = !b;                           // (the first positions to arrive get fitted: set())
  if (!b) {                                  // nothing to show: the whole world, centred
    this.z = Math.max(1, Math.min(this.maxZ(), Math.ceil(Math.log(w / 256) / Math.LN2)));
    this.cx = this.cy = 128 * Math.pow(2, this.z);
    return;
  }
  var z = Math.min(this.maxZ(), 17);
  while (z > 1 && (wx(b[3], z) - wx(b[1], z) > w - 60 || wy(b[0], z) - wy(b[2], z) > hh - 60)) --z;
  this.z = z;
  this.cx = (wx(b[1], z) + wx(b[3], z)) / 2; this.cy = (wy(b[0], z) + wy(b[2], z)) / 2;
};
TileMap.prototype.set = function (layers, key) {
  this.layers = layers;
  var ts = tileset();
  if (ts !== this.ts) { this.ts = ts; this.tiles.textContent = ''; this.imgs = {}; this.ok = this.bad = 0; }
  if (key !== this.key && this.el.clientWidth) { this.key = key; this.fit(); }
  else if (key !== this.key) this.pendingKey = key;
  else if (this.empty && this.el.clientWidth && this.bounds()) this.fit();   // first positions on an empty map
  this.draw();
};
TileMap.prototype.draw = function () {
  var m = this, w = m.el.clientWidth, hh = m.el.clientHeight;
  if (!w || !hh) { if (m.el.isConnected && !m.wait) { m.wait = true; requestAnimationFrame(function () { m.wait = false; m.draw(); }); } return; }
  if (m.pendingKey !== undefined) { m.key = m.pendingKey; m.pendingKey = undefined; m.fit(); }
  var ts = m.ts || tileset(), z = m.z, n = Math.pow(2, z), x0 = m.cx - w / 2, y0 = m.cy - hh / 2;
  // tiles
  var want = {};
  if (ts && ts.url) {
    for (var tx = Math.floor(x0 / 256); tx <= Math.floor((x0 + w) / 256); tx++)
      for (var ty = Math.floor(y0 / 256); ty <= Math.floor((y0 + hh) / 256); ty++) {
        if (ty < 0 || ty >= n) continue;
        var k = z + '/' + tx + '/' + ty, img = m.imgs[k];
        want[k] = 1;
        if (!img) {
          var xx = ((tx % n) + n) % n, sub = ts.sub ? ts.sub.charAt((xx + ty) % ts.sub.length) : '';
          img = h('img', { alt: '', draggable: 'false', src: ts.url.replace('{z}', z).replace('{x}', xx).replace('{y}', ty).replace('{s}', sub) });
          img.addEventListener('load', function () { m.ok++; m.status(); });
          img.addEventListener('error', function () { this.style.visibility = 'hidden'; m.bad++; m.status(); });
          m.imgs[k] = img; m.tiles.appendChild(img);
        }
        img.style.left = (tx * 256 - x0) + 'px'; img.style.top = (ty * 256 - y0) + 'px';
      }
  }
  for (var key in m.imgs) if (!want[key]) { m.imgs[key].remove(); delete m.imgs[key]; }
  m.att.textContent = ts && ts.url ? ts.attrib : '';
  m.att.hidden = !(ts && ts.url && ts.attrib);
  // overlay
  var svg = m.svg;
  svg.textContent = '';
  svg.setAttribute('width', w); svg.setAttribute('height', hh);
  var P = function (ll) { return [wx(ll[1], z) - x0, wy(ll[0], z) - y0]; };
  // Everything of one radio (layer item .g) is a group: hovering any of it
  // highlights the group (m.hover, or else the selected one, m.focus).
  var grp = function (el, g) {
    if (g == null) return el;
    el.setAttribute('data-g', g);
    el.addEventListener('mouseenter', function () { m.hover = g; m.hl(); });
    el.addEventListener('mouseleave', function () { if (m.hover === g) { m.hover = null; m.hl(); } });
    return el;
  };
  var tip = function (el, t) { if (t) { var e = document.createElementNS(SVGNS, 'title'); e.textContent = t; el.appendChild(e); } };
  m.layers.lines.forEach(function (l) {
    if (l.pts.length < 2) return;
    var pts = l.pts.map(function (p) { var q = P(p); return q[0].toFixed(1) + ',' + q[1].toFixed(1); }).join(' ');
    grp(svg.appendChild(sv('polyline', { class: 'tm-line' + (l.sel ? ' sel' : ''), stroke: l.color || '#5bc0de', points: pts })), l.g);
    var q0 = P(l.pts[0]);
    grp(svg.appendChild(sv('circle', { class: 'tm-start', cx: q0[0].toFixed(1), cy: q0[1].toFixed(1), r: 3.5, stroke: l.color || '#5bc0de' })), l.g);
    if (l.onclick) {
      var hit = grp(sv('polyline', { class: 'tm-hit', points: pts }), l.g);
      tip(hit, l.title);
      hit.addEventListener('click', l.onclick);
      svg.appendChild(hit);
    }
  });
  m.layers.pts.forEach(function (p) {
    var q = P(p.ll);
    if (q[0] < -50 || q[1] < -50 || q[0] > w + 50 || q[1] > hh + 50) return;
    var c = grp(sv('circle', { class: 'tm-pt', cx: q[0].toFixed(1), cy: q[1].toFixed(1), r: p.r || 5, fill: p.color || '#5bc0de' }), p.g);
    tip(c, p.title);
    if (p.onclick) c.addEventListener('click', p.onclick);
    svg.appendChild(c);
    if (p.label) {
      var tx = sv('text', { x: (q[0] + (p.r || 5) + 3).toFixed(1), y: (q[1] + 4).toFixed(1) });
      tx.textContent = p.label;
      if (p.g != null) tx.setAttribute('data-g', p.g);
      svg.appendChild(tx);
    }
  });
  // The hover highlight follows what is under the pointer now (the old
  // elements are gone -- e.g. a click that selected the radio redrew it).
  var under = m.px != null && document.elementFromPoint(m.px, m.py);
  var ug = under && under.closest ? under.closest('[data-g]') : null;
  m.hover = ug && svg.contains(ug) ? ug.getAttribute('data-g') : null;
  m.hl();
  m.status();
};
// Highlight one radio's path, dot and label (the hovered one, else the
// layers' focus -- the selected radio), fading the rest.
TileMap.prototype.hl = function () {
  var g = this.hover != null ? this.hover : this.layers.focus, svg = this.svg;
  svg.classList.toggle('hl', g != null);
  var els = svg.querySelectorAll('[data-g]');
  for (var i = 0; i < els.length; i++) els[i].classList.toggle('on', g != null && els[i].getAttribute('data-g') === String(g));
};
TileMap.prototype.status = function () {
  var ts = this.ts || tileset();
  var off = !ts || !ts.url, failed = !off && this.bad > 0 && this.ok === 0;
  this.msg.hidden = !failed && !(off && !this.opts.quietOff);
  this.msg.textContent = failed ? 'The map tiles didn\u2019t load (offline?) \u2014 pick another map, or set DSD_NET_MAP_TILES' :
                         'No map tiles (positions only)';
};
// The Map view: each listed radio's latest position (labelled) and path;
// the selected radio's path stands out, with every fix.
var MV = null;
function viewMap() {
  if (!MV) {
    MV = new TileMap();
    $('mwrap').appendChild(MV.el);
    var sel = $('m-tiles'), ts = tilesets();
    for (var k in ts) sel.appendChild(h('option', { value: k }, ts[k].name));
    sel.value = tileKey();
    sel.addEventListener('change', function () { store('map.tiles', sel.value); renderView(); renderDetail(); });
    $('m-paths').addEventListener('change', function () { renderView(); });
    $('m-fit').addEventListener('click', function () { MV.fit(); MV.draw(); });
  }
  var rs = fRadios().filter(function (r) { return trackOf(r).length; });
  var selId = S.sel && S.sel.type === 'radio' ? S.sel.id : null, lines = [], pts = [];
  rs.forEach(function (r) {
    var tr = trackOf(r), col = nodeColor({ ref: r }), me = r.id === selId;
    var last = tr[tr.length - 1], al = r.aliases.length ? ' ' + r.aliases[r.aliases.length - 1] : '';
    var pick = function () { select('radio', r.id); };
    if ($('m-paths').checked || me)
      lines.push({ pts: tr.map(function (f) { return ll(f[1]); }), color: col, sel: me, g: r.id, onclick: pick,
                   title: 'Radio ' + r.id + al + ' \u2014 path of ' + tr.length + ' positions, ' + hms(tr[0][0]) + ' \u2192 ' + hms(last[0]) + ' (click to select)' });
    if (me) tr.slice(0, -1).forEach(function (f) { pts.push({ ll: ll(f[1]), r: 3, color: col, g: r.id, title: 'Radio ' + r.id + ' ' + hms(f[0]) + '  ' + f[1] }); });
    pts.push({ ll: ll(last[1]), r: me ? 7 : 5.5, color: col, label: r.id + al, g: r.id,
               title: 'Radio ' + r.id + al + ' \u2014 ' + tr.length + ' position' + (tr.length > 1 ? 's' : '') + ', latest ' + hms(last[0]) + ' (' + ago(last[0]) + ')',
               onclick: pick });
  });
  MV.set({ lines: lines, pts: pts, focus: selId }, S.fam + '|' + (S.file || 'live'));
  $('m-note').textContent = rs.length ? rs.length + ' radio' + (rs.length > 1 ? 's' : '') + ' with positions' +
    (anyFilter() || S.q ? ' (with the filters)' : '') + ' \u00B7 drag to pan, scroll to zoom, click a radio' : 'No position reports among the listed radios yet.';
}
// The map in a radio's details (kept across re-renders so it doesn't reload).
var DMAP = null;
function radioMap(r) {
  var tr = trackOf(r), col = nodeColor({ ref: r }), id = S.fam + '|' + r.id;
  if (!DMAP || DMAP.id !== id) DMAP = { id: id, map: new TileMap({ quietOff: true }) };
  var pts = tr.map(function (f, i) {
    return { ll: ll(f[1]), r: i === tr.length - 1 ? 6 : i === 0 ? 5 : 3.5, color: i === tr.length - 1 ? '#d9534f' : i === 0 ? '#5cb85c' : col,
             title: hms(f[0]) + '  ' + f[1] };
  });
  DMAP.map.set({ lines: [{ pts: tr.map(function (f) { return ll(f[1]); }), color: col }], pts: pts }, id);
  return DMAP.map.el;
}

// ---- position history ----
function trackOf(r) { return r.track && r.track.length ? r.track : r.pos ? [[r.pos_t, r.pos]] : []; }
function ll(p) { var a = p.split(','); return [+a[0], +a[1]]; }
// Metres between two fixes (equirectangular; fine at these distances).
function metres(a, b) {
  var k = Math.PI / 180, x = (b[1] - a[1]) * k * Math.cos((a[0] + b[0]) / 2 * k), y = (b[0] - a[0]) * k;
  return Math.sqrt(x * x + y * y) * 6371000;
}
function dist(m) { return m < 1000 ? Math.round(m) + ' m' : (m / 1000).toFixed(m < 10000 ? 2 : 1) + ' km'; }
// A small sketch of a radio's track: no map, just its shape -- oldest fix
// green, latest red -- and how far it spans.
function trackSketch(tr) {
  var W = 300, H = 150, P = 12, pts = tr.map(function (f) { return ll(f[1]); });
  var la0 = 1e9, la1 = -1e9, lo0 = 1e9, lo1 = -1e9;
  pts.forEach(function (p) { la0 = Math.min(la0, p[0]); la1 = Math.max(la1, p[0]); lo0 = Math.min(lo0, p[1]); lo1 = Math.max(lo1, p[1]); });
  var kx = Math.cos((la0 + la1) / 2 * Math.PI / 180), sx = (lo1 - lo0) * kx, sy = la1 - la0;
  var sc = Math.min((W - 2 * P) / (sx || 1e-9), (H - 2 * P) / (sy || 1e-9));
  if (!sx && !sy) sc = 0;
  var X = function (p) { return (W / 2 + ((p[1] - (lo0 + lo1) / 2) * kx) * sc).toFixed(1); };
  var Y = function (p) { return (H / 2 - (p[0] - (la0 + la1) / 2) * sc).toFixed(1); };
  var svg = sv('svg', { class: 'trk', viewBox: '0 0 ' + W + ' ' + H, width: '100%', role: 'img', 'aria-label': 'Track sketch' });
  if (pts.length > 1) svg.appendChild(sv('polyline', { points: pts.map(function (p) { return X(p) + ',' + Y(p); }).join(' '), class: 'trkline' }));
  pts.forEach(function (p, i) {
    var c = sv('circle', { cx: X(p), cy: Y(p), r: i === pts.length - 1 ? 4.5 : i === 0 ? 4 : 2.5,
                           class: i === pts.length - 1 ? 'trkend' : i === 0 ? 'trkstart' : 'trkpt' });
    var t = document.createElementNS(SVGNS, 'title'); t.textContent = hms(tr[i][0]) + '  ' + tr[i][1];
    c.appendChild(t); svg.appendChild(c);
  });
  var span = metres([la0, lo0], [la1, lo1]), moved = 0;
  for (var i = 1; i < pts.length; i++) moved += metres(pts[i - 1], pts[i]);
  return h('div', { class: 'trkbox' }, [svg, h('div', { class: 'hint', text: pts.length + ' fixes \u00B7 spans ' + dist(span) +
    ' \u00B7 moved ' + dist(moved) + ' \u00B7 ' + hms(tr[0][0]) + '\u2013' + hms(tr[tr.length - 1][0]) + ' (UTC)' })]);
}
function xmlEsc(s) { return String(s).replace(/[<>&"']/g, function (c) { return { '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;', "'": '&apos;' }[c]; }); }
// KML of radios' tracks: a line through each radio's fixes and a timestamped
// point per fix (Google Earth, Google My Maps' Import).
function tracksKml(radios, title) {
  var o = ['<?xml version="1.0" encoding="UTF-8"?>', '<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>' + xmlEsc(title) + '</name>'];
  radios.forEach(function (r) {
    var tr = trackOf(r);
    if (!tr.length) return;
    var name = 'Radio ' + r.id + (r.aliases.length ? ' ' + r.aliases[r.aliases.length - 1] : '');
    var c = function (p) { var q = ll(p); return q[1] + ',' + q[0] + ',0'; };
    o.push('<Folder><name>' + xmlEsc(name) + '</name>');
    if (tr.length > 1)
      o.push('<Placemark><name>' + xmlEsc(name) + ' track</name><LineString><tessellate>1</tessellate><coordinates>' +
             tr.map(function (f) { return c(f[1]); }).join(' ') + '</coordinates></LineString></Placemark>');
    tr.forEach(function (f) {
      o.push('<Placemark><name>' + xmlEsc(name + ' ' + hms(f[0])) + '</name><TimeStamp><when>' + new Date(f[0]).toISOString() +
             '</when></TimeStamp><Point><coordinates>' + c(f[1]) + '</coordinates></Point></Placemark>');
    });
    o.push('</Folder>');
  });
  o.push('</Document></kml>');
  return o.join('\n');
}
function saveText(name, text, type) {
  var a = h('a', { href: URL.createObjectURL(new Blob([text], { type: type })), download: name });
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(function () { URL.revokeObjectURL(a.href); }, 60000);
}
// A radio's details: its positions -- the latest, a sketch of the track, the
// fixes (newest first), a Google Maps route and a KML download.
function positionsSection(d, r) {
  var tr = trackOf(r);
  d.appendChild(h('h4', { text: 'Positions (' + tr.length + ')' }));
  d.appendChild(h('div', { class: 'posline' }, [posLink(r.pos), h('span', { class: 'hint', text: ' ' + ago(r.pos_t) })]));
  if (tr.length < 2) {
    if (tileset() && tileset().url) { d.appendChild(radioMap(r)); DMAP.map.draw(); }
    d.appendChild(h('div', { class: 'hint', text: 'Every report so far is from this one place; a path appears once it reports from somewhere else.' }));
    return;
  }
  var ts = tileset();
  if (ts && ts.url) {
    var el = radioMap(r);
    d.appendChild(el);
    DMAP.map.draw();
    var sk = trackSketch(tr);                     // its summary line (fixes, span, distance, times)
    d.appendChild(sk.lastChild);
  } else d.appendChild(trackSketch(tr));
  d.appendChild(h('div', { class: 'tagrow' }, [
    h('a', { class: 'btn sm', href: '#', title: 'All radios\u2019 positions on the Map view',
             onclick: function (e) { e.preventDefault(); setView('map'); } }, 'Map view'),
    h('button', { class: 'btn sm', type: 'button', title: 'The exact track and fixes, for Google Earth or Google My Maps (Import)',
                  onclick: function () { saveText('radio_' + safeName(r.id) + '_track.kml', tracksKml([r], 'Radio ' + r.id), 'application/vnd.google-earth.kml+xml'); } },
      'Download KML')]));
  var ul = h('ul', { class: 'lst' });
  tr.slice().reverse().slice(0, 25).forEach(function (f) {
    ul.appendChild(h('li', null, [h('div', null, posLink(f[1])), h('span', { class: 'c', text: hms(f[0]) })]));
  });
  d.appendChild(ul);
  if (tr.length > 25) d.appendChild(h('div', { class: 'hint', text: 'and ' + (tr.length - 25) + ' earlier (all of them in the KML)' }));
}
// The Content column: the message, else the data service; and a position report.
function textCell(c) {
  var parts = [];
  if (c.text) parts.push(c.text);
  else if (c.svc) parts.push(h('span', { class: 'svc', title: svcTip(c.svc), text: svcLabel(c.svc) }));
  else if (c.kid) parts.push(h('span', { class: 'svc', title: 'Encrypted with the key this call named (algorithm \u00B7 key id)',
                                         text: keyText(c.alg, c.kid) }));
  if (c.pos) { if (parts.length) parts.push(' '); parts.push(posLink(c.pos)); }
  return parts.length ? h('span', null, parts) : '';
}
function sttSpan(c, cls) {
  var r = c.audio && ASR.tx[c.audio];
  return r && r.t ? h(cls === 'tx' ? 'div' : 'span', { class: 'stt' + (cls ? ' ' + cls : ''), text: r.t,
                       title: 'Speech-to-text (' + r.m.replace(/^.*\//, '') + '); may be wrong' }) : null;
}

// ---------- now playing ----------
function showNp() {
  var c = PLAYER.call;
  if (!c) return;
  $('np').hidden = false;
  document.body.classList.add('np-on');
  var m = $('npmeta');
  m.textContent = '';
  m.appendChild(h('b', { class: 'mono', text: hms(c.start) + 'Z' }));
  if (c.freq) m.appendChild(h('span', { class: 'mono', text: mhz(c.freq) + ' MHz' }));
  if (c.slot) m.appendChild(h('span', { text: 'slot ' + c.slot }));
  m.appendChild(h('span', null, [c.src ? rlink(c.src, c.alias || null) : '?', ' → ', toCell(c)]));
  if (c.net) m.appendChild(netc(c.net));
  m.appendChild(h('span', { id: 'nptime', class: 'mono' }));
  $('npdl').href = audioUrl(c);
  $('npdl').setAttribute('download', callFile(c));
  npProgress();
  npText();
}
function npProgress() {
  var a = PLAYER.a, c = PLAYER.call;
  if (!a || !c || $('np').hidden) return;
  var d = isFinite(a.duration) && a.duration > 0 ? a.duration : (c.audio_ms || 0) / 1000, t = a.currentTime || 0;
  if (!PLAYER.name && a.ended) t = d;
  $('npprog').style.width = d ? Math.min(100, 100 * t / d) + '%' : '0';
  var el = $('nptime');
  if (el) el.textContent = mmss(t * 1000) + ' / ' + mmss(d * 1000);
}
function closeNp() {
  if (PLAYER.a && PLAYER.name) PLAYER.a.pause();
  PLAYER.name = null; PLAYER.call = null;
  $('np').hidden = true;
  document.body.classList.remove('np-on');
  syncPlay();
}
// The transcript line: the text, or what speech-to-text is doing for this call.
function npText() {
  var c = PLAYER.call, el = $('nptx');
  if (!c || $('np').hidden) return;
  el.textContent = '';
  el.className = 'tx st';
  var r = ASR.tx[c.audio], j = ASR.job && ASR.job.name === c.audio ? ASR.job : ASR.next && ASR.next.name === c.audio ? ASR.next : null;
  if (r && !j) {
    el.className = 'tx' + (r.t ? '' : ' st');
    el.appendChild(document.createTextNode(r.t || '(no clear speech recognized)'));
    el.appendChild(h('span', { class: 'alias', text: '  · ' + r.m.replace(/^.*\//, '') + (r.ms ? ' · ' + (r.ms / 1000).toFixed(1) + ' s' : '') +
                                                  (r.p ? ' · partial (call still in progress)' : '') }));
    return;
  }
  if (!ASR.on) { el.textContent = 'Transcribe on play is off (Calls toolbar).'; return; }
  if (ASR.state === 'nosrc') {
    el.appendChild(document.createTextNode('Speech-to-text needs its files on the server (tools/get_asr_assets.sh) — or '));
    el.appendChild(h('a', { href: '#', onclick: function (e) {
      e.preventDefault(); ASR.net = true; store('asr.net', '1'); ASR.state = 'off'; asrPump(); npText();
    } }, 'load them from the internet'));
    el.appendChild(document.createTextNode(' (jsDelivr and Hugging Face, ~100 MB, then cached).'));
    return;
  }
  if (ASR.state === 'error') { el.textContent = 'Speech-to-text failed: ' + ASR.err; return; }
  if (ASR.lastErr && ASR.lastErr.name === c.audio && !j) { el.textContent = 'Could not transcribe this call: ' + ASR.lastErr.err; return; }
  if (!j) { el.textContent = ''; return; }
  if (ASR.state === 'loading')
    el.textContent = 'Loading the speech model' + (ASR.loaded ? ' — ' + mb(ASR.loaded) : '') + ' (once per visit)…';
  else el.textContent = j === ASR.job && j.phase === 'run' ? 'Transcribing…' : 'Waiting to transcribe…';
}

// ---------- speech-to-text ----------
// Whisper runs in this browser (a worker: /net/asr_worker.js), on demand --
// a call is transcribed when it is played. The library and model come from
// the server (/net/asr/, filled by tools/get_asr_assets.sh) or, only if the
// user says so, from the internet. Transcripts are kept in this browser.
var LANGS = [['english', 'English'], ['auto', 'Detect language'], ['spanish', 'Spanish'], ['french', 'French'],
             ['german', 'German'], ['italian', 'Italian'], ['portuguese', 'Portuguese'], ['dutch', 'Dutch'],
             ['polish', 'Polish'], ['russian', 'Russian'], ['ukrainian', 'Ukrainian'], ['arabic', 'Arabic'],
             ['chinese', 'Chinese'], ['japanese', 'Japanese'], ['korean', 'Korean'], ['vietnamese', 'Vietnamese'],
             ['turkish', 'Turkish'], ['hindi', 'Hindi']];
var CDN_LIB = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@4.3.0/dist/transformers.min.js';
var ASR = { cfg: null, cfgP: null, on: load('asr') !== '0', lang: load('asr.lang'), model: load('asr.model'),
            net: load('asr.net') === '1', w: null, wModel: null, state: 'off', loaded: 0, total: 0, err: '',
            job: null, next: null, seq: 0, lastErr: null, tx: {} };
try { ASR.tx = JSON.parse(load('asr.tx') || '{}') || {}; } catch (e) { ASR.tx = {}; }
function txSave() {
  var k = Object.keys(ASR.tx);
  if (k.length > 1000) k.slice(0, k.length - 800).forEach(function (x) { delete ASR.tx[x]; });
  var keep = {};
  Object.keys(ASR.tx).forEach(function (x) { if (!ASR.tx[x].p) keep[x] = ASR.tx[x]; });
  store('asr.tx', JSON.stringify(keep));
}
// Whisper's stock inventions on silence and noise; its loops (below).
var JUNK = /^(you|thank you|thanks for watching|thank you for watching|thank you so much for watching|please subscribe|subtitles by .*|.*amara\.org.*)$/i;
function cleanTx(t) {
  t = String(t || '').replace(/\[[^\]]*\]|\([^)]*\)|\*[^*]*\*|\u266A/g, ' ').replace(/\s+/g, ' ').trim();
  // A phrase said 4+ times in a row is Whisper stuck in a loop, not speech.
  if (/(^|\s)(\S+(?:\s+\S+){0,3}?)(?:[\s,.!?]+\2(?=[\s,.!?]|$)){3,}/i.test(t)) return '';
  if (!/[0-9A-Za-z\u00C0-\uFFFF]/.test(t)) return '';
  return JUNK.test(t.replace(/[\s.!?,…]+$/, '')) ? '' : t;
}
function asrCfg() {
  if (!ASR.cfgP)
    ASR.cfgP = fetch('/net/asr/config.json', { cache: 'no-store' }).then(function (r) { return r.json(); })
      .then(function (c) { ASR.cfg = c; asrControls(); return c; }, function (e) { ASR.cfgP = null; throw e; });
  return ASR.cfgP;
}
function asrModel() {
  var c = ASR.cfg;
  return ASR.model && (!c.local || c.models.indexOf(ASR.model) >= 0) ? ASR.model : c.model;
}
function asrLang() { return ASR.lang || (ASR.cfg && ASR.cfg.language) || 'english'; }
function asrSource() {
  if (ASR.cfg.local) return { lib: '/net/asr/transformers.min.js', wasm: '/net/asr/ort/', local: '/net/asr/models/' };
  if (ASR.net) return { lib: CDN_LIB, wasm: null, local: null };
  return null;
}
function asrControls() {
  var c = ASR.cfg, ls = $('asrlang'), ms = $('asrmodel'), model = asrModel();
  if (!ls.options.length)
    LANGS.forEach(function (l) { ls.appendChild(h('option', { value: l[0] }, l[1])); });
  ls.value = asrLang();
  ls.hidden = /\.en$/.test(model);
  ms.textContent = '';
  var list = c.local ? c.models : [c.model];
  list.forEach(function (m) { ms.appendChild(h('option', { value: m }, m.replace(/^.*\//, ''))); });
  ms.value = model;
  ms.hidden = list.length < 2;
}
function asrFail(msg) {
  ASR.state = 'error'; ASR.err = msg;
  if (ASR.w) ASR.w.terminate();
  ASR.w = null; ASR.wModel = null; ASR.job = null; ASR.next = null;
  npText();
}
function asrWorker(model) {
  if (ASR.w) ASR.w.terminate();
  var src = asrSource();
  ASR.w = new Worker('/net/asr_worker.js', { type: 'module' });
  ASR.wModel = model; ASR.state = 'loading'; ASR.loaded = 0; ASR.total = 0; ASR.err = '';
  ASR.w.onmessage = asrMsg;
  ASR.w.onerror = function (e) { asrFail('the speech-to-text worker stopped' + (e && e.message ? ' (' + e.message + ')' : '')); };
  ASR.w.postMessage({ cmd: 'load', lib: new URL(src.lib, location.href).href,
                      wasm: src.wasm ? new URL(src.wasm, location.href).href : null, local: src.local, model: model });
}
function asrMsg(e) {
  var m = e.data, j = ASR.job;
  if (m.type === 'progress') { ASR.loaded = m.loaded; ASR.total = m.total; npText(); return; }
  if (m.type === 'ready') { ASR.state = 'ready'; ASR.threads = m.threads; asrPump(); return; }
  if (m.type === 'error' && m.cmd === 'load') { asrFail(m.msg); return; }
  if (!j || j.id !== m.id) return;
  ASR.job = null;
  if (m.type === 'result') {
    ASR.tx[j.name] = { t: cleanTx(m.text), m: j.model, l: j.lang, ms: m.ms, p: j.live || undefined };
    txSave();
    if (S.view === 'calls' && IX) renderView();
  } else if (m.type === 'error') {
    j.err = m.msg; ASR.lastErr = j;
  }
  npText();
  asrPump();
}
// Transcribe call `c` (the newest request wins; one runs at a time).
function transcribe(c) {
  if (!c.audio || S.file) return;
  ASR.next = { id: ++ASR.seq, c: c, name: c.audio, live: live(c) };
  asrCfg().then(function () {
    var r = ASR.tx[c.audio], n = ASR.next;
    if (n && n.name === c.audio && r && !r.p && r.m === asrModel() && r.l === asrLang()) ASR.next = null;   // already done
    asrPump();
    npText();
  }, function () { ASR.next = null; ASR.state = 'error'; ASR.err = 'the server did not answer'; npText(); });
  npText();
}
function asrPump() {
  if (ASR.job || !ASR.next || !ASR.cfg) return;
  if (!asrSource()) { ASR.state = 'nosrc'; npText(); return; }
  var model = asrModel();
  if (!ASR.w || ASR.wModel !== model) asrWorker(model);
  if (ASR.state !== 'ready') { npText(); return; }
  var j = ASR.job = ASR.next;
  ASR.next = null;
  j.model = model; j.lang = asrLang(); j.phase = 'decode';
  npText();
  fetch(audioUrl(j.c), { cache: 'no-store' }).then(function (r) {
    if (!r.ok) throw new Error('its audio is no longer on the server');
    return r.arrayBuffer();
  }).then(function (buf) {
    // Decoding into a 16 kHz context also resamples (Whisper's rate).
    return new OfflineAudioContext(1, 16000, 16000).decodeAudioData(buf);
  }).then(function (ab) {
    if (ASR.job !== j || !ASR.w) return;
    j.phase = 'run';
    npText();
    var pcm = ab.getChannelData(0).slice();
    ASR.w.postMessage({ cmd: 'run', id: j.id, pcm: pcm,
                        language: /\.en$/.test(j.model) || j.lang === 'auto' ? null : j.lang }, [pcm.buffer]);
  }).catch(function (e) {
    if (ASR.job !== j) return;
    ASR.job = null; j.err = String((e && e.message) || e); ASR.lastErr = j;
    npText();
    asrPump();
  });
}

// ---------- audio download (.zip of the listed calls) ----------
var CRCT = null;
function crc32(u8) {
  if (!CRCT) {
    CRCT = new Uint32Array(256);
    for (var n = 0; n < 256; n++) { var c = n; for (var k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; CRCT[n] = c >>> 0; }
  }
  var x = 0xFFFFFFFF;
  for (var i = 0; i < u8.length; i++) x = CRCT[(x ^ u8[i]) & 255] ^ (x >>> 8);
  return (x ^ 0xFFFFFFFF) >>> 0;
}
// A zip (stored, not compressed: WAV barely compresses) of [{name, data}].
function zipBlob(files) {
  var parts = [], cdir = [], off = 0, cdLen = 0, enc = new TextEncoder(), d = new Date();
  var dtime = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  var ddate = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  files.forEach(function (f) {
    var nm = enc.encode(f.name), crc = crc32(f.data), n = f.data.length;
    var lh = new DataView(new ArrayBuffer(30));
    lh.setUint32(0, 0x04034b50, true); lh.setUint16(4, 20, true); lh.setUint16(6, 0x0800, true);
    lh.setUint16(10, dtime, true); lh.setUint16(12, ddate, true); lh.setUint32(14, crc, true);
    lh.setUint32(18, n, true); lh.setUint32(22, n, true); lh.setUint16(26, nm.length, true);
    parts.push(lh.buffer, nm, f.data);
    var ch = new DataView(new ArrayBuffer(46));
    ch.setUint32(0, 0x02014b50, true); ch.setUint16(4, 20, true); ch.setUint16(6, 20, true); ch.setUint16(8, 0x0800, true);
    ch.setUint16(12, dtime, true); ch.setUint16(14, ddate, true); ch.setUint32(16, crc, true);
    ch.setUint32(20, n, true); ch.setUint32(24, n, true); ch.setUint16(28, nm.length, true); ch.setUint32(42, off, true);
    cdir.push(ch.buffer, nm);
    off += 30 + nm.length + n;
    cdLen += 46 + nm.length;
  });
  var e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, cdLen, true); e.setUint32(16, off, true);
  return new Blob(parts.concat(cdir, [e.buffer]), { type: 'application/zip' });
}
function csvCell(v) {
  v = v == null ? '' : String(v);
  if (/^[=+\-@]/.test(v)) v = "'" + v;
  return /[",\r\n]/.test(v) ? '"' + v.replace(/"/g, '""') + '"' : v;
}
var ZIP_MAX = 25 * 1048576;
function zipCalls() {
  var btn = $('zip');
  if (btn.disabled) return;
  var rows = (S.rows.calls || []).filter(function (c) { return hasAudio(c) && !live(c); });
  if (!rows.length) { toast('No finished calls with audio in the list.'); return; }
  btn.disabled = true;
  var label = btn.textContent, files = [], names = {}, total = 0, i = 0, missing = 0, full = false;
  var csv = [['file', 'start_utc', 'duration_s', 'audio_s', 'network', 'mhz', 'slot', 'from', 'alias', 'to', 'private',
              'emergency', 'encrypted', 'transcript', 'transcript_model']];
  function next() {
    if (i >= rows.length || full) return Promise.resolve();
    var c = rows[i++];
    btn.textContent = 'Zipping ' + i + ' / ' + rows.length + '…';
    return fetch(audioUrl(c)).then(function (r) { if (!r.ok) throw new Error(); return r.arrayBuffer(); }).then(function (b) {
      if (total + b.byteLength > ZIP_MAX) { full = true; return; }
      var name = callFile(c), t = ASR.tx[c.audio], k = 1;
      while (names[name]) name = callFile(c).replace(/\.wav$/, '_' + (++k) + '.wav');
      names[name] = 1;
      total += b.byteLength;
      files.push({ name: name, data: new Uint8Array(b) });
      csv.push([name, dt(c.start), ((c.last - c.start) / 1000).toFixed(1), ((c.audio_ms || 0) / 1000).toFixed(1), netLabel(c.net),
                c.freq ? mhz(c.freq) : '', c.slot || '', c.src || '', c.alias || '', c.tgt || '', c.priv ? 'yes' : '',
                c.emerg ? 'yes' : '', c.enc ? 'yes' : '', t ? t.t : '', t ? t.m : '']);
    }, function () { ++missing; }).then(next);
  }
  next().then(function () {
    if (!files.length) { toast('None of the listed calls’ audio is still on the server.'); return; }
    var n = files.length;
    files.push({ name: 'calls.csv', data: new TextEncoder().encode('﻿' + csv.map(function (r) { return r.map(csvCell).join(','); }).join('\r\n') + '\r\n') });
    var a = h('a', { href: URL.createObjectURL(zipBlob(files)), download: 'dsd_calls_' + stamp(now()) + '.zip' });
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 60000);
    toast('Saved ' + n + ' call' + (n > 1 ? 's' : '') + ' (' + mb(total) + ')' +
          (full ? ' — the first ' + n + ' of ' + rows.length + ' listed; the zip stops at ' + mb(ZIP_MAX) : '') +
          (missing ? '; ' + missing + ' no longer on the server' : '') + '.');
  }, function (e) { toast('Could not make the zip (' + e + ').'); }).then(function () { btn.disabled = false; btn.textContent = label; });
}

// ---------- calls list ----------
// A call as a card (phones).
function callCard(c) {
  var pb = playBtn(c);
  return h('div', { class: 'ccard' }, [
    h('div', { class: 'r1' }, [h('span', { class: 't mono', text: hms(c.start) }), durCell(c), typeBadges(c), pb, pb ? dlLink(c) : null]),
    h('div', { class: 'r2' }, [c.src ? rlink(c.src, c.alias || null) : '—', ' → ', toCell(c)]),
    h('div', { class: 'r3' }, [c.net ? netc(c.net) : null,
                               c.freq && !(IX.netByKey[c.net] && IX.netByKey[c.net].label.indexOf(mhz(c.freq)) >= 0)
                                 ? h('span', { class: 'mono', text: mhz(c.freq) + ' MHz' }) : null,
                               c.slot ? h('span', { text: 'slot ' + c.slot }) : null]),
    c.text || c.svc || c.pos ? h('div', { class: 'tx' }, textCell(c)) : null,
    sttSpan(c, 'tx')]);
}
function viewCalls() {
  var rows = fCalls();
  var anyAud = (S.audio && S.audio.on && !S.file) || S.audOnly || IX.calls.some(hasAudio);
  document.querySelectorAll('.callbar .audctl').forEach(function (e) { e.classList.toggle('off', !anyAud); });
  table($('t-calls'), 'calls', [
    { label: 'Start (UTC)', cls: 'mono nowrap', k: function (c) { return c.start; }, cell: function (c) { return hms(c.start); } },
    { label: 'Duration', cls: 'nowrap', k: function (c) { return c.last - c.start; }, cell: durCell },
    { label: 'Network', cell: function (c) { return c.net ? netc(c.net, c.freq) : '-'; } },
    { label: 'MHz', cls: 'mono nowrap', k: function (c) { return c.freq || 0; }, cell: function (c) { return mhz(c.freq) || '—'; },
      dropEmpty: function (c) { return !!c.freq; } },
    { label: 'Slot', cls: 'num', k: function (c) { return c.slot; }, cell: function (c) { return c.slot || '—'; },
      dropEmpty: function (c) { return !!c.slot; } },
    { label: 'From', k: function (c) { return c.src; }, cell: function (c) { return c.src ? rlink(c.src, c.alias || null) : '—'; } },
    { label: 'To', cls: 'tgcell', k: function (c) { return c.tgt; }, cell: toCell },
    { label: 'Type', cls: 'ctype', cell: typeBadges },
    { label: 'Audio', cls: 'nowrap', k: function (c) { return hasAudio(c) ? 1 : 0; }, cell: audioCell,
      hideEmpty: hasAudio },
    { label: 'Content', cls: 'wrap', cell: textCell, hideEmpty: function (c) { return !!(c.text || c.svc || c.pos || c.kid); } }
  ], rows, (S.audOnly ? 'No calls with audio' : 'No calls heard yet') + (anyFilter() || S.q ? ' for this filter.' : '.'),
     null, null, callCard, function (c) { return sttSpan(c); });
}
function viewTgs() {
  var rows = fTgs().slice().sort(function (a, b) { return b.last - a.last; });
  table($('t-tgs'), 'tgs', [
    { label: 'Talkgroup', k: function (t) { return t.id; }, dir: 1, cell: function (t) { return tlink(t.id); } },
    { label: 'Networks', cell: function (t) { return nets(t.networks); } },
    { label: 'Radios', cls: 'num', k: function (t) { return keys(t.radios).length; }, cell: function (t) { return keys(t.radios).length; } },
    { label: 'Calls', cls: 'num', k: function (t) { return t.calls; }, cell: function (t) { return t.calls; } },
    { label: 'Emerg.', cls: 'num', k: function (t) { return t.emerg; }, cell: function (t) { return t.emerg ? badge('b-emerg', t.emerg) : '—'; } },
    { label: 'Encr.', cls: 'num', k: function (t) { return t.enc; }, cell: function (t) { return t.enc ? badge('b-enc', t.enc) : '—'; } },
    { label: 'First heard', cls: 'nowrap', k: function (t) { return t.first; }, cell: function (t) { return ago(t.first); } },
    { label: 'Last heard', cls: 'nowrap', k: function (t) { return t.last; }, cell: function (t) { return ago(t.last); } }
  ], rows, 'No talkgroups yet.', function (t) { select('tg', t.id); }, function (t) { return isSel('tg', t.id); });
}
function viewRadios() {
  var rows = fRadios().slice().sort(function (a, b) { return b.last - a.last; });
  table($('t-radios'), 'radios', [
    { label: 'Radio', k: function (r) { return r.id; }, dir: 1, cell: function (r) { return rlink(r.id); } },
    { label: 'Talkgroups', cell: function (r) {
        var ts = keys(r.tgs).sort(function (a, b) { return r.tgs[b] - r.tgs[a]; });
        var w = h('span');
        ts.slice(0, 4).forEach(function (t, i) { if (i) w.appendChild(document.createTextNode(', ')); w.appendChild(tlink(t)); });
        if (ts.length > 4) w.appendChild(h('span', { class: 'alias', text: ' +' + (ts.length - 4) }));
        return ts.length ? w : '—';
      } },
    { label: '# TGs', cls: 'num', k: function (r) { return keys(r.tgs).length; }, cell: function (r) { return keys(r.tgs).length; } },
    { label: 'Private peers', cls: 'num', k: function (r) { return keys(r.peers).length; }, cell: function (r) { return keys(r.peers).length || '—'; } },
    { label: 'Networks', cell: function (r) { return nets(r.networks); } },
    { label: 'Calls', cls: 'num', k: function (r) { return r.calls; }, cell: function (r) { return r.calls; } },
    { label: 'Last heard', cls: 'nowrap', k: function (r) { return r.last; }, cell: function (r) { return ago(r.last); } }
  ], rows, 'No radios yet.', function (r) { select('radio', r.id); }, function (r) { return isSel('radio', r.id); });
}
function viewNets() {
  var rows = IX.nets.slice().sort(function (a, b) { return b.last - a.last; });
  table($('t-nets'), 'nets', [
    { label: 'Network', k: function (n) { return n.label; }, dir: 1, cell: function (n) {
        var m = (n.parts || []).length - 1;
        return m > 0 ? h('span', null, [netc(n.key), h('span', { class: 'alias', style: 'white-space:nowrap', title: 'Networks merged into this one (see its details)',
                                                                text: ' +' + m + ' merged' })]) : netc(n.key); } },
    { label: 'Identity', cell: function (n) { return confBadge(n.confidence); } },
    { label: 'Identifiers', cls: 'ids', hideMd: true, cell: function (n) { return keys(n.ids).map(function (k) { return k + '=' + n.ids[k]; }).join('  '); } },
    { label: 'Sites', cell: function (n) { return n.sites.join(', ') || '—'; } },
    { label: 'Channels (MHz)', cls: 'mono', cell: function (n) { return (n.freqs || []).map(mhz).join(', ') || '—'; },
      dropEmpty: function (n) { return !!(n.freqs && n.freqs.length); } },
    { label: 'TGs', cls: 'num', k: function (n) { return IX.netTg[n.key] || 0; }, cell: function (n) { return IX.netTg[n.key] || 0; } },
    { label: 'Radios', cls: 'num', k: function (n) { return IX.netRad[n.key] || 0; }, cell: function (n) { return IX.netRad[n.key] || 0; } },
    { label: 'Calls', cls: 'num', k: function (n) { return n.calls; }, cell: function (n) { return n.calls; } },
    { label: 'Streams', cls: 'num', hideMd: true, k: function (n) { return n.sessions; }, cell: function (n) { return n.sessions; } },
    { label: 'Last heard', cls: 'nowrap', k: function (n) { return n.last; }, cell: function (n) { return ago(n.last); } }
  ], rows, 'No networks identified yet.', function (n) { select('net', n.key); }, function (n) { return isSel('net', n.key); });
}

// Connected components of the radio-talkgroup(-radio) graph: groups of radios
// tied together through the talkgroups they share (and private calls).
function communities(tgs, radios) {
  var par = {};
  function f(x) { while (par[x] !== x) { par[x] = par[par[x]]; x = par[x]; } return x; }
  function u(a, b) { a = f(a); b = f(b); if (a !== b) par[a] = b; }
  var tset = {}, rset = {};
  tgs.forEach(function (t) { tset[t.id] = 1; par['t:' + t.id] = 't:' + t.id; });
  radios.forEach(function (r) { rset[r.id] = 1; par['r:' + r.id] = 'r:' + r.id; });
  radios.forEach(function (r) {
    keys(r.tgs).forEach(function (t) { if (tset[t]) u('r:' + r.id, 't:' + t); });
    keys(r.peers).forEach(function (p) { if (rset[p]) u('r:' + r.id, 'r:' + p); });
  });
  var g = {};
  Object.keys(par).forEach(function (n) {
    var root = f(n);
    (g[root] = g[root] || { t: [], r: [] })[n[0]].push(n.slice(2));
  });
  return Object.keys(g).map(function (k) { return g[k]; })
    .filter(function (c) { return c.r.length >= 2 || (c.r.length >= 1 && c.t.length >= 1); })
    .sort(function (a, b) { return (b.r.length + b.t.length) - (a.r.length + a.t.length); });
}
function viewLinks() {
  var cont = $('t-links'), tgs = fTgs(), radios = fRadios();
  var y = cont.scrollTop;
  cont.textContent = '';
  // 1. Communities
  var hadSame = sameSection(cont);
  var comms = communities(tgs, radios);
  cont.appendChild(h('div', { class: 'section', text: 'Talk communities — radios tied together through shared talkgroups / private calls (' + comms.length + ')' }));
  if (!comms.length) cont.appendChild(h('div', { class: 'note', text: 'No linked radios yet.' }));
  var grid = h('div', { class: 'grid2' });
  comms.slice(0, 60).forEach(function (c, i) {
    var netset = {};
    c.t.forEach(function (t) { (IX.tgById[t] || { networks: [] }).networks.forEach(function (k) { netset[k] = 1; }); });
    c.r.forEach(function (r) { (IX.rById[r] || { networks: [] }).networks.forEach(function (k) { netset[k] = 1; }); });
    var tl = c.t.slice().sort(function (a, b) { return ((IX.tgById[b] || {}).calls || 0) - ((IX.tgById[a] || {}).calls || 0); });
    var rl = c.r.slice().sort(function (a, b) { return ((IX.rById[b] || {}).calls || 0) - ((IX.rById[a] || {}).calls || 0); });
    var te = h('div', { class: 'ents' }), re = h('div', { class: 'ents' });
    tl.slice(0, 30).forEach(function (t) { te.appendChild(tlink(t)); });
    rl.slice(0, 40).forEach(function (r) { re.appendChild(rlink(r)); });
    if (rl.length > 40) re.appendChild(h('span', { class: 'alias', text: '+' + (rl.length - 40) + ' more' }));
    grid.appendChild(h('div', { class: 'comm' }, [
      h('h5', { text: 'Community ' + (i + 1) }),
      h('div', { class: 'meta' }, [c.r.length + ' radios · ' + c.t.length + ' talkgroups · ', nets(Object.keys(netset))]),
      c.t.length ? te : null, h('div', { style: 'height:.35rem' }), re]));
  });
  cont.appendChild(grid);
  // 2. Cross-network
  var xt = tgs.filter(function (t) { return t.networks.length > 1; });
  var xr = radios.filter(function (r) { return r.networks.length > 1; });
  cont.appendChild(h('div', { class: 'section', text: 'Seen on more than one network — possible links between networks (' + (xt.length + xr.length) + ')' }));
  if (!xt.length && !xr.length) cont.appendChild(h('div', { class: 'note', text: 'Nothing shared between networks yet. A talkgroup or radio heard on two networks shows up here.' }));
  else {
    var ul = h('div', { class: 'grid2' });
    xt.forEach(function (t) { ul.appendChild(h('div', { class: 'comm' }, [h('h5', null, tlink(t.id)), h('div', { class: 'meta' }, [t.calls + ' calls · ' + keys(t.radios).length + ' radios · ', nets(t.networks)])])); });
    xr.forEach(function (r) { ul.appendChild(h('div', { class: 'comm' }, [h('h5', null, rlink(r.id)), h('div', { class: 'meta' }, [r.calls + ' calls · ', nets(r.networks)])])); });
    cont.appendChild(ul);
  }
  // 3. Hubs
  var hubs = radios.filter(function (r) { return keys(r.tgs).length >= 3; })
                   .sort(function (a, b) { return keys(b.tgs).length - keys(a.tgs).length; });
  cont.appendChild(h('div', { class: 'section', text: 'Hub radios — active on 3+ talkgroups (dispatchers, supervisors, scanning radios) (' + hubs.length + ')' }));
  if (!hubs.length) cont.appendChild(h('div', { class: 'note', text: 'None yet.' }));
  else {
    var hb = h('div', { class: 'grid2' });
    hubs.slice(0, 40).forEach(function (r) {
      var ts = keys(r.tgs).sort(function (a, b) { return r.tgs[b] - r.tgs[a]; });
      var te = h('div', { class: 'ents' });
      ts.slice(0, 20).forEach(function (t) { te.appendChild(tlink(t)); });
      hb.appendChild(h('div', { class: 'comm' }, [h('h5', null, rlink(r.id)), h('div', { class: 'meta', text: ts.length + ' talkgroups · ' + r.calls + ' calls' }), te]));
    });
    cont.appendChild(hb);
  }
  if (!hadSame && IX.nets.length > 1) {
    cont.appendChild(h('div', { class: 'section', text: 'Probably the same network — merge suggestions (0)' }));
    cont.appendChild(h('div', { class: 'note', text: 'None: no two networks look like one channel heard at different frequency offsets.' }));
  }
  cont.appendChild(h('div', { style: 'height:.8rem' }));
  cont.scrollTop = y;
}

// ---------- detail panel ----------
function lst(items, max) {
  var ul = h('ul', { class: 'lst' });
  var top = items.length ? items[0].n : 1;
  items.slice(0, max || 25).forEach(function (it) {
    ul.appendChild(h('li', null, [h('div', null, [it.el, h('div', { class: 'bar', style: 'width:' + Math.max(4, 100 * it.n / top) + '%' })]),
                                  h('span', { class: 'c', text: it.lbl != null ? it.lbl : it.n })]));
  });
  if (items.length > (max || 25)) ul.appendChild(h('li', null, [h('span', { class: 'alias', text: '+' + (items.length - (max || 25)) + ' more' }), h('span')]));
  return items.length ? ul : h('div', { class: 'hint', text: 'None.' });
}
function kv(pairs) { return h('div', { class: 'kv' }, pairs.map(function (p) { return h('div', null, [h('b', { text: String(p[1]) }), h('span', { text: p[0] })]); })); }
// Under a recent call: its text (quoted), else what the data call carried;
// and a position report -- as the Calls view's Content column shows them.
function recentNote(c) {
  var kids = [];
  if (c.text) kids.push('\u201C' + c.text + '\u201D');
  else if (c.svc) kids.push(h('span', { class: 'svc', title: svcTip(c.svc), text: svcLabel(c.svc) }));
  if (c.pos) { if (kids.length) kids.push(' '); kids.push(posLink(c.pos)); }
  return kids.length ? h('div', { class: 'alias' }, kids) : null;
}
function recent(filter) {
  var cs = IX.calls.filter(filter).slice(0, 12);
  if (!cs.length) return h('div', { class: 'hint', text: 'No calls in the recent buffer.' });
  var ul = h('ul', { class: 'lst' });
  cs.forEach(function (c) {
    var pb = playBtn(c);
    ul.appendChild(h('li', null, [
      h('div', null, [c.src ? rlink(c.src, c.alias || null) : '?', ' → ', c.tgt ? (c.priv ? rlink(c.tgt) : tlink(c.tgt)) : '?',
                      c.emerg ? h('span', null, [' ', badge('b-emerg', 'EMERG')]) : null,
                      recentNote(c), sttSpan(c, 'tx')]),
      h('span', { class: 'c' }, [pb, pb ? ' ' : '', live(c) ? 'live' : hms(c.start)])]));
  });
  return ul;
}
// The details are rebuilt on every update: keep where the panel was scrolled
// to while the same item stays selected (a new selection starts at the top).
function renderDetail(force) {
  if (!force && mergePickBusy()) return;
  var d = $('detail'), sel = S.sel, who = sel ? sel.type + ':' + sel.id : '';
  var y = who === renderDetail.who ? d.scrollTop : 0, wy = window.scrollY;
  renderDetail.who = who;
  // (the panel keeps its height while it is rebuilt: emptied, it could
  // shorten the page and pull the page's scroll position up)
  d.style.minHeight = d.offsetHeight + 'px';
  detailBody(d);
  d.style.minHeight = '';
  d.scrollTop = y;
  if (window.scrollY !== wy) window.scrollTo(window.scrollX, wy);
}
function detailBody(d) {
  d.textContent = '';
  var sel = S.sel;
  if (!sel) document.body.classList.remove('sheet');
  d.appendChild(h('div', { class: 'sheet-x' }, h('button', { class: 'btn', type: 'button', onclick: closeSheet }, '\u2715 Close')));
  d.classList.toggle('has-sel', !!sel);
  // Clear the selection (back to the overview text); Esc does the same.
  if (sel) d.appendChild(h('button', { class: 'desel', type: 'button', title: 'Deselect (Esc)', 'aria-label': 'Deselect',
                                       onclick: closeSheet }, '\u2715'));
  if (!sel) {
    d.appendChild(h('div', { class: 'hint' }, [h('h3', { text: 'Associations' }),
      'Click a radio, talkgroup or network anywhere on the page to see what it is tied to: the talkgroups a radio uses, ' +
      'who talks on a talkgroup, which talkgroups share radios, private-call partners, and the networks they were heard on.']));
    return;
  }
  if (sel.type === 'tg') {
    var t = IX.tgById[sel.id];
    d.appendChild(h('h3', { text: 'Talkgroup ' + sel.id }));
    if (!t) { d.appendChild(h('div', { class: 'hint', text: 'Not in the current data.' })); return; }
    d.appendChild(h('div', { class: 'tagrow' }, t.networks.map(netc)));
    d.appendChild(filterBtns('tgf', t.id, 'talkgroup', 'Hide this talkgroup\u2019s calls (and radios heard only on it)'));
    d.appendChild(kv([['Calls', t.calls], ['Radios', keys(t.radios).length], ['Emergency', t.emerg],
                      ['Encrypted', t.enc], ['First', ago(t.first)], ['Last', ago(t.last)]]));
    keysSection(d, t.keys, 'on it', { nets: t.networks });
    d.appendChild(h('h4', { text: 'Radios on this talkgroup' }));
    d.appendChild(lst(keys(t.radios).sort(function (a, b) { return t.radios[b] - t.radios[a]; })
      .map(function (r) { return { el: rlink(r), n: t.radios[r] }; })));
    // Talkgroups tied to this one through the radios they share.
    var linked = {};
    keys(t.radios).forEach(function (r) {
      var R = IX.rById[r];
      if (R) keys(R.tgs).forEach(function (o) { if (o !== sel.id) linked[o] = (linked[o] || 0) + 1; });
    });
    d.appendChild(h('h4', { text: 'Linked talkgroups (shared radios)' }));
    d.appendChild(lst(keys(linked).sort(function (a, b) { return linked[b] - linked[a]; })
      .map(function (o) { return { el: tlink(o), n: linked[o], lbl: linked[o] + ' shared' }; })));
    d.appendChild(h('h4', { text: 'Recent calls' }));
    d.appendChild(recent(function (c) { return !c.priv && c.tgt === sel.id; }));
  } else if (sel.type === 'radio') {
    var r = IX.rById[sel.id];
    d.appendChild(h('h3', { text: 'Radio ' + sel.id }));
    if (!r) { d.appendChild(h('div', { class: 'hint', text: 'Not in the current data.' })); return; }
    if (r.aliases.length) d.appendChild(h('div', { class: 'alias', text: 'Alias: ' + r.aliases.join(' / ') }));
    d.appendChild(h('div', { class: 'tagrow' }, r.networks.map(netc)));
    d.appendChild(filterBtns('rf', r.id, 'radio', 'Hide this radio and its calls'));
    d.appendChild(kv([['Calls', r.calls], ['Talkgroups', keys(r.tgs).length], ['Private peers', keys(r.peers).length],
                      ['Networks', r.networks.length], ['First', ago(r.first)], ['Last', ago(r.last)]]));
    if (r.pos) positionsSection(d, r);
    keysSection(d, r.keys, 'from it', { nets: r.networks });
    d.appendChild(h('h4', { text: 'Talkgroups used' }));
    d.appendChild(lst(keys(r.tgs).sort(function (a, b) { return r.tgs[b] - r.tgs[a]; })
      .map(function (t) { return { el: tlink(t), n: r.tgs[t] }; })));
    d.appendChild(h('h4', { text: 'Private calls with' }));
    d.appendChild(lst(keys(r.peers).sort(function (a, b) { return r.peers[b] - r.peers[a]; })
      .map(function (p) { return { el: rlink(p), n: r.peers[p] }; })));
    // Radios this one shares talkgroups with.
    var co = {};
    keys(r.tgs).forEach(function (t) {
      var T = IX.tgById[t];
      if (T) keys(T.radios).forEach(function (o) { if (o !== sel.id) co[o] = (co[o] || 0) + 1; });
    });
    d.appendChild(h('h4', { text: 'Shares talkgroups with' }));
    d.appendChild(lst(keys(co).sort(function (a, b) { return co[b] - co[a]; })
      .map(function (o) { return { el: rlink(o), n: co[o], lbl: co[o] + ' TG' + (co[o] > 1 ? 's' : '') }; }), 20));
    d.appendChild(h('h4', { text: 'Recent calls' }));
    d.appendChild(recent(function (c) { return c.src === sel.id || (c.priv && c.tgt === sel.id); }));
  } else if (sel.type === 'net') {
    var n = IX.netByKey[sel.id];
    if (!n) { d.appendChild(h('h3', { text: 'Network' })); d.appendChild(h('div', { class: 'hint', text: 'Not in the current data.' })); return; }
    d.appendChild(h('h3', null, [sw(n.key), ' ', n.label]));
    d.appendChild(h('div', { class: 'tagrow' }, [confBadge(n.confidence),
      h('button', { class: 'btn sm', type: 'button', title: onlyNet(n.key) ? 'Show every network again' : 'Show only this network',
                    onclick: function () { setNets(onlyNet(n.key) ? [] : [n.key]); } }, onlyNet(n.key) ? 'All' : 'Only'),
      h('button', { class: 'btn sm', type: 'button',
                    title: S.nets[n.key] === 'out' ? 'Show this network again' : 'Hide this network\u2019s calls, talkgroups and radios',
                    onclick: function () { excludeNet(n.key); } }, S.nets[n.key] === 'out' ? 'Include' : 'Exclude')]));
    d.appendChild(kv([['Calls', n.calls], ['Talkgroups', IX.netTg[n.key] || 0], ['Radios', IX.netRad[n.key] || 0],
                      ['Streams', n.sessions], ['First', ago(n.first)], ['Last', ago(n.last)]]));
    d.appendChild(h('h4', { text: 'Identifiers' }));
    d.appendChild(h('div', { class: 'ids', text: keys(n.ids).map(function (k) { return k + '=' + n.ids[k]; }).join('   ') || '—' }));
    d.appendChild(h('h4', { text: 'Sites' }));
    d.appendChild(h('div', { text: n.sites.join(', ') || 'None reported.' }));
    d.appendChild(h('h4', { text: 'Channels' }));
    d.appendChild(h('div', { class: 'mono', text: (n.freqs || []).map(function (f) { return mhz(f) + ' MHz'; }).join(', ') ||
      'Unknown (the client sent no center_freq).' }));
    keysSection(d, n.keys, 'on it', { nets: (n.parts || [n]).map(function (p) { return p.key; }), net: n.key });
    mergeSection(d, n);
    if (n.confidence !== 'strong')
      d.appendChild(h('div', { class: 'hint', style: 'margin-top:.6rem;font-size:.8rem',
        text: n.confidence === 'channel'
          ? 'Channel identity: a short code (color code / NAC / RAN), or nothing, heard on a known frequency -- in practice one conventional channel or repeater. Every stream on that frequency with that code shares it, and merged data from other receivers joins it too (unless DSD_NET_CHANNEL_MERGE=receiver).'
          : n.confidence === 'weak'
          ? 'Weak identity: only a short code (color code / NAC / RAN) was seen, which unrelated systems can share, and the stream\'s frequency is unknown, so this bucket covers just one stream. Talkgroups and radios it shares with other buckets are listed under Links.'
          : 'No network identity has been decoded on this stream yet.' }));
    d.appendChild(h('h4', { text: 'Busiest talkgroups' }));
    d.appendChild(lst(IX.tgs.filter(function (t) { return t.networks.indexOf(n.key) >= 0; })
      .sort(function (a, b) { return b.calls - a.calls; }).map(function (t) { return { el: tlink(t.id), n: t.calls }; }), 15));
    d.appendChild(h('h4', { text: 'Busiest radios' }));
    d.appendChild(lst(IX.radios.filter(function (r) { return r.networks.indexOf(n.key) >= 0; })
      .sort(function (a, b) { return b.calls - a.calls; }).map(function (r) { return { el: rlink(r.id), n: r.calls }; }), 15));
  }
}

// ---------- graph (self-contained force layout on SVG) ----------
var SVGNS = 'http://www.w3.org/2000/svg';
function sv(tag, a) { var e = document.createElementNS(SVGNS, tag); for (var k in a) e.setAttribute(k, a[k]); return e; }
var GR = { nodes: [], links: [], by: {}, adj: {}, sig: '', t: { k: 1, x: 0, y: 0 }, alpha: 0, raf: 0, fitted: false, userView: false,
           root: null, lg: null, ng: null, drag: null, pan: null, pts: {}, pinch: null };
function gSig() {
  return [S.d.version, S.fam, netKeys().sort().map(function (k) { return S.nets[k] + ':' + k; }).join(','),
          keys(S.tgf).sort().map(function (k) { return S.tgf[k] + ':' + k; }).join(','),
          keys(S.rf).sort().map(function (k) { return S.rf[k] + ':' + k; }).join(','), S.q, $('g-cap').value, $('g-priv').checked].join('|');
}
function buildGraph() {
  var sig = gSig();
  if (sig === GR.sig) return;
  var famChanged = GR.sig.split('|')[1] !== S.fam;
  GR.sig = sig;
  var cap = +$('g-cap').value, priv = $('g-priv').checked;
  var tgs = fTgs(), radios = fRadios();
  var cand = tgs.map(function (t) { return { id: 't:' + t.id, kind: 'tg', ref: t, score: t.calls * 2 + keys(t.radios).length }; })
    .concat(radios.map(function (r) { return { id: 'r:' + r.id, kind: 'radio', ref: r, score: r.calls + keys(r.tgs).length }; }));
  cand.sort(function (a, b) { return b.score - a.score; });
  var keep = {};
  cand.slice(0, cap).forEach(function (c) { keep[c.id] = c; });
  var links = [], deg = {};
  function add(a, b, w, p) { links.push({ a: a, b: b, w: w, priv: p }); deg[a] = (deg[a] || 0) + 1; deg[b] = (deg[b] || 0) + 1; }
  radios.forEach(function (r) {
    var rid = 'r:' + r.id;
    if (!keep[rid]) return;
    keys(r.tgs).forEach(function (t) { if (keep['t:' + t]) add(rid, 't:' + t, r.tgs[t], false); });
    if (priv) keys(r.peers).forEach(function (p) { if (r.id < p && keep['r:' + p]) add(rid, 'r:' + p, r.peers[p], true); });
  });
  var old = famChanged ? {} : GR.by, by = {}, nodes = [];
  Object.keys(keep).forEach(function (id) {
    if (!deg[id] && keep[id].kind === 'radio' && Object.keys(keep).length > 30) return; // drop lone radios when busy
    var c = keep[id], o = old[id];
    var nd = o || { id: id, x: (Math.random() - .5) * 300, y: (Math.random() - .5) * 300, vx: 0, vy: 0 };
    nd.kind = c.kind; nd.ref = c.ref; nd.isNew = !o;
    by[id] = nd; nodes.push(nd);
  });
  links = links.filter(function (l) { return by[l.a] && by[l.b]; });
  // Seed new nodes next to an already-placed neighbour so updates don't explode.
  GR.adj = {};
  links.forEach(function (l) { (GR.adj[l.a] = GR.adj[l.a] || []).push(l.b); (GR.adj[l.b] = GR.adj[l.b] || []).push(l.a); });
  nodes.forEach(function (n) {
    if (!n.isNew) return;
    var nb = (GR.adj[n.id] || []).map(function (x) { return by[x]; }).filter(function (x) { return x && !x.isNew; })[0];
    if (nb) { n.x = nb.x + (Math.random() - .5) * 40; n.y = nb.y + (Math.random() - .5) * 40; }
  });
  // Layout mass: 1 + links. Repulsion scales with both ends' mass, so a hub
  // (a talkgroup, or a radio with many private-call partners) pushes harder
  // and its neighbours ring it instead of other nodes being trapped inside.
  nodes.forEach(function (n) { n.m = 1 + (GR.adj[n.id] || []).length; n.r = nodeR(n); });
  GR.nodes = nodes; GR.links = links; GR.by = by;
  $('g-note').textContent = nodes.length + ' nodes · ' + links.length + ' links' +
    (cand.length > cap ? ' (busiest ' + cap + ' of ' + cand.length + ')' : '');
  drawGraph();
  // Every voice frame bumps the model version; only re-heat the layout when
  // the node/link set itself changed, so a busy call doesn't keep it jiggling.
  var structure = nodes.map(function (x) { return x.id; }).sort().join(',') + '|' +
                  links.map(function (l) { return l.a + '>' + l.b; }).sort().join(',');
  if (famChanged) { GR.fitted = false; GR.userView = false; }
  if (structure !== GR.structure || famChanged) {
    GR.structure = structure;
    GR.alpha = Math.max(GR.alpha, famChanged || !GR.fitted ? 1 : 0.35);
  }
  run();
}
function nodeColor(n) { var k = primaryNet(n.ref.networks, n.id); return k ? colorFor(k) : '#7a8288'; }
function nodeR(n) {
  return n.kind === 'tg' ? 7 + Math.min(14, Math.sqrt(n.ref.calls) * 2) : 4 + Math.min(6, Math.sqrt(n.ref.calls));
}
function drawGraph() {
  var svg = $('gsvg');
  svg.textContent = '';
  GR.root = sv('g', {}); GR.lg = sv('g', {}); GR.ng = sv('g', {});
  GR.root.appendChild(GR.lg); GR.root.appendChild(GR.ng); svg.appendChild(GR.root);
  var showLbl = $('g-labels').checked;
  GR.links.forEach(function (l) {
    l.el = sv('line', { class: 'glink' + (l.priv ? ' priv' : ''), 'stroke-width': Math.min(5, 1 + Math.log2(1 + l.w)) });
    GR.lg.appendChild(l.el);
  });
  GR.nodes.forEach(function (n) {
    var g = sv('g', { class: 'gnode ' + n.kind + (n.ref.networks.length > 1 ? ' multi' : '') });
    var r = nodeR(n), col = nodeColor(n), shape;
    if (n.kind === 'tg') shape = sv('rect', { class: 'shape', x: -r, y: -r, width: 2 * r, height: 2 * r, rx: 3, fill: col });
    else shape = sv('circle', { class: 'shape', r: r, fill: col });
    g.appendChild(shape);
    var ttl = document.createElementNS(SVGNS, 'title');
    var al = n.kind === 'radio' && n.ref.aliases.length ? ' (' + n.ref.aliases.join(' / ') + ')' : '';
    ttl.textContent = (n.kind === 'tg' ? 'Talkgroup ' : 'Radio ') + n.ref.id + al + ' — ' + n.ref.calls + ' calls';
    g.appendChild(ttl);
    if (n.kind === 'tg' || showLbl) {
      var label = n.kind === 'tg' ? 'TG ' + n.ref.id : n.ref.id + (n.ref.aliases.length ? ' ' + n.ref.aliases[n.ref.aliases.length - 1] : '');
      var tx = sv('text', { x: r + 3, y: 3.5 });
      tx.textContent = label;
      g.appendChild(tx);
    }
    g.addEventListener('pointerdown', function (e) {
      // (the svg's own handler, next, tracks the pointer for pinch-zoom)
      if (!GR.pinch && !Object.keys(GR.pts).length)
        GR.drag = { n: n, moved: false, sx: e.clientX, sy: e.clientY };
    });
    n.el = g;
    GR.ng.appendChild(g);
  });
  highlight();
  paint();
}
function highlight() {
  var svg = $('gsvg'), sel = S.sel, id = sel ? (sel.type === 'tg' ? 't:' : sel.type === 'radio' ? 'r:' : '') + sel.id : null;
  var focus = id && GR.by[id];
  svg.classList.toggle('focus', !!focus);
  var nb = {};
  if (focus) { nb[id] = 1; (GR.adj[id] || []).forEach(function (x) { nb[x] = 1; }); }
  GR.nodes.forEach(function (n) {
    if (!n.el) return;
    n.el.classList.toggle('hl', !!nb[n.id]);
    n.el.classList.toggle('sel', n.id === id);
  });
  GR.links.forEach(function (l) { if (l.el) l.el.classList.toggle('hl', !!(focus && (l.a === id || l.b === id))); });
}
function paint() {
  var svg = $('gsvg'), w = svg.clientWidth || 800, hh = svg.clientHeight || 600;
  if (GR.root) GR.root.setAttribute('transform', 'translate(' + (w / 2 + GR.t.x) + ',' + (hh / 2 + GR.t.y) + ') scale(' + GR.t.k + ')');
  GR.links.forEach(function (l) {
    var a = GR.by[l.a], b = GR.by[l.b];
    l.el.setAttribute('x1', a.x.toFixed(1)); l.el.setAttribute('y1', a.y.toFixed(1));
    l.el.setAttribute('x2', b.x.toFixed(1)); l.el.setAttribute('y2', b.y.toFixed(1));
  });
  GR.nodes.forEach(function (n) { n.el.setAttribute('transform', 'translate(' + n.x.toFixed(1) + ',' + n.y.toFixed(1) + ')'); });
}
function step() {
  var N = GR.nodes, L = GR.links, a = GR.alpha, n = N.length, i, j;
  for (i = 0; i < n; i++) {
    var A = N[i];
    for (j = i + 1; j < n; j++) {
      var B = N[j], dx = B.x - A.x, dy = B.y - A.y, d2 = dx * dx + dy * dy, mm = A.m * B.m;
      if (d2 > 160000 * Math.min(4, Math.max(1, mm / 4))) continue;
      if (d2 < 1) { dx = Math.random() - .5; dy = Math.random() - .5; d2 = 1; }
      var d = Math.sqrt(d2), f = 500 * mm * a / d2;
      // Never on top of each other: closer than their sizes plus a gap
      // (more beside a talkgroup, whose label is bold) pushes apart hard.
      var gap = A.r + B.r + (A.kind === 'tg' || B.kind === 'tg' ? 14 : 8);
      if (d < gap) f += (gap - d) * 0.5;
      var ux = dx / d * f, uy = dy / d * f;
      A.vx -= ux; A.vy -= uy; B.vx += ux; B.vy += uy;
    }
  }
  L.forEach(function (l) {
    var A = GR.by[l.a], B = GR.by[l.b], dx = B.x - A.x, dy = B.y - A.y, d = Math.sqrt(dx * dx + dy * dy) || 1;
    // Private-call links pull less than talkgroup use: the talkgroups shape
    // the picture, a radio's private partners gather around it.
    var rest = l.priv ? 60 : 75, f = (d - rest) * (l.priv ? 0.04 : 0.05) * a * Math.min(2, 0.6 + Math.log2(1 + l.w) * 0.3);
    var ux = dx / d * f, uy = dy / d * f;
    A.vx += ux; A.vy += uy; B.vx -= ux; B.vy -= uy;
  });
  for (i = 0; i < n; i++) {
    var q = N[i];
    q.vx -= q.x * 0.008 * a; q.vy -= q.y * 0.008 * a;
    if (GR.drag && GR.drag.n === q) { q.vx = q.vy = 0; continue; }
    q.vx *= 0.55; q.vy *= 0.55;
    q.x += Math.max(-30, Math.min(30, q.vx)); q.y += Math.max(-30, Math.min(30, q.vy));
  }
  GR.alpha *= 0.985;
}
function run() {
  if (GR.raf) return;
  (function frame() {
    if (S.view !== 'graph' || GR.alpha < 0.012) {
      // Settled -- but finish gliding the view to fit, if it is still on its way.
      if (S.view === 'graph' && GR.fitted && autoFit()) { paint(); GR.raf = requestAnimationFrame(frame); return; }
      GR.raf = 0;
      if (S.view === 'graph' && !GR.fitted && GR.nodes.length) { fit(); GR.fitted = true; }
      return;
    }
    for (var k = 0; k < 2; k++) step();
    if (!GR.fitted && GR.alpha < 0.2 && GR.nodes.length) { fit(); GR.fitted = true; }
    else if (GR.fitted) autoFit();
    paint();
    GR.raf = requestAnimationFrame(frame);
  })();
}
// The view that shows every node: { k, x, y } (null with no nodes).
function fitView() {
  var svg = $('gsvg'), w = svg.clientWidth || 800, hh = svg.clientHeight || 600;
  if (!GR.nodes.length) return null;
  var x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  GR.nodes.forEach(function (n) { x0 = Math.min(x0, n.x); y0 = Math.min(y0, n.y); x1 = Math.max(x1, n.x); y1 = Math.max(y1, n.y); });
  // Cap the zoom so a small graph isn't blown up until its labels collide.
  var k = Math.min(1.35, Math.max(0.15, Math.min(w / (x1 - x0 + 160), hh / (y1 - y0 + 100))));
  return { k: k, x: -(x0 + x1) / 2 * k, y: -(y0 + y1) / 2 * k };
}
// Fit (the Fit button): show every node -- and keep doing so (autoFit) until
// the user zooms or pans.
function fit() {
  var v = fitView();
  GR.userView = false;
  if (!v) return;
  GR.t = v;
  paint();
}
// Until the user zooms or pans on their own (GR.userView), the view follows
// the graph as it grows: when a node would be drawn outside the frame, the
// view glides (a step per frame) towards the one that fits everything.
function autoFit() {
  if (GR.userView || GR.drag || GR.pinch || !GR.nodes.length) return false;
  var svg = $('gsvg'), w = svg.clientWidth, hh = svg.clientHeight;
  if (!w || !hh) return false;
  var t = GR.t, out = false, m = 12;
  for (var i = 0; i < GR.nodes.length && !out; i++) {
    var n = GR.nodes[i], sx = w / 2 + t.x + n.x * t.k, sy = hh / 2 + t.y + n.y * t.k;
    out = sx < m || sy < m || sx > w - m || sy > hh - m;
  }
  if (!out && !GR.easing) return false;
  var v = fitView();
  if (!v) return false;
  var f = 0.2;
  t.k += (v.k - t.k) * f; t.x += (v.x - t.x) * f; t.y += (v.y - t.y) * f;
  // Keep easing until the view has arrived (not just until nodes are back in).
  GR.easing = Math.abs(v.k - t.k) / v.k > 0.01 || Math.abs(v.x - t.x) > 2 || Math.abs(v.y - t.y) > 2;
  if (!GR.easing) { GR.t = v; }
  return true;
}
function toGraph(cx, cy) {
  var svg = $('gsvg'), r = svg.getBoundingClientRect();
  return { x: (cx - r.left - r.width / 2 - GR.t.x) / GR.t.k, y: (cy - r.top - r.height / 2 - GR.t.y) / GR.t.k };
}
// Zoom to scale k1 keeping the point (cx, cy) -- relative to the svg centre --
// where it is.
function zoomAt(cx, cy, k1) {
  k1 = Math.max(0.1, Math.min(6, k1));
  var k0 = GR.t.k;
  GR.t.x = cx - (cx - GR.t.x) * k1 / k0; GR.t.y = cy - (cy - GR.t.y) * k1 / k0; GR.t.k = k1;
  paint();
}
(function wireGraph() {
  var svg = $('gsvg');
  function centred(x, y) { var r = svg.getBoundingClientRect(); return { x: x - r.left - r.width / 2, y: y - r.top - r.height / 2 }; }
  function two() {
    var ids = Object.keys(GR.pts), a = GR.pts[ids[0]], b = GR.pts[ids[1]];
    return { d: Math.max(1, Math.hypot(a.x - b.x, a.y - b.y)), m: centred((a.x + b.x) / 2, (a.y + b.y) / 2) };
  }
  svg.addEventListener('pointerdown', function (e) {
    GR.pts[e.pointerId] = { x: e.clientX, y: e.clientY };
    svg.setPointerCapture(e.pointerId);
    if (Object.keys(GR.pts).length === 2) {
      // Second finger: pinch-zoom (and pan with the midpoint) instead of a drag.
      var g = two();
      GR.pinch = { d0: g.d, m0: g.m, k0: GR.t.k, x0: GR.t.x, y0: GR.t.y };
      GR.userView = true; GR.easing = false;
      GR.drag = null; GR.pan = null;
      svg.classList.remove('panning');
      return;
    }
    if (GR.drag || GR.pinch) return;
    GR.pan = { x: e.clientX, y: e.clientY, tx: GR.t.x, ty: GR.t.y, moved: false };
    svg.classList.add('panning');
  });
  svg.addEventListener('pointermove', function (e) {
    if (GR.pts[e.pointerId]) GR.pts[e.pointerId] = { x: e.clientX, y: e.clientY };
    if (GR.pinch) {
      if (Object.keys(GR.pts).length < 2) return;
      var g = two(), P = GR.pinch, k1 = Math.max(0.1, Math.min(6, P.k0 * g.d / P.d0));
      GR.t.k = k1;
      GR.t.x = g.m.x - (P.m0.x - P.x0) * k1 / P.k0;
      GR.t.y = g.m.y - (P.m0.y - P.y0) * k1 / P.k0;
      paint();
    } else if (GR.drag) {
      var dg = GR.drag;
      if (Math.abs(e.clientX - dg.sx) + Math.abs(e.clientY - dg.sy) > 3) dg.moved = true;
      if (!dg.moved) return;
      var p = toGraph(e.clientX, e.clientY);
      dg.n.x = p.x; dg.n.y = p.y;
      GR.alpha = Math.max(GR.alpha, 0.25); run(); paint();
    } else if (GR.pan) {
      var dx = e.clientX - GR.pan.x, dy = e.clientY - GR.pan.y;
      if (Math.abs(dx) + Math.abs(dy) > 3) { GR.pan.moved = true; GR.userView = true; GR.easing = false; }
      GR.t.x = GR.pan.tx + dx; GR.t.y = GR.pan.ty + dy; paint();
    }
  });
  function up(e) {
    delete GR.pts[e.pointerId];
    if (GR.pinch) {
      // A pinch ends when a finger lifts; the other one may carry on panning
      // (never counted as a tap).
      GR.pinch = null;
      var rest = Object.keys(GR.pts);
      if (rest.length === 1) {
        var q = GR.pts[rest[0]];
        GR.pan = { x: q.x, y: q.y, tx: GR.t.x, ty: GR.t.y, moved: true };
      }
      return;
    }
    if (GR.drag) {
      var dg = GR.drag; GR.drag = null;
      if (!dg.moved) select(dg.n.kind === 'tg' ? 'tg' : 'radio', dg.n.ref.id);
    } else if (GR.pan) {
      if (!GR.pan.moved && S.sel) { if (drawer()) closeSheet(); else { S.sel = null; renderDetail(); highlight(); } }
      GR.pan = null;
    }
    svg.classList.remove('panning');
  }
  svg.addEventListener('pointerup', up);
  svg.addEventListener('pointercancel', up);
  svg.addEventListener('wheel', function (e) {
    e.preventDefault();
    var c = centred(e.clientX, e.clientY);
    GR.userView = true; GR.easing = false;
    zoomAt(c.x, c.y, GR.t.k * Math.exp(-e.deltaY * 0.0015));
  }, { passive: false });
  $('g-zin').addEventListener('click', function () { GR.userView = true; GR.easing = false; zoomAt(0, 0, GR.t.k * 1.4); });
  $('g-zout').addEventListener('click', function () { GR.userView = true; GR.easing = false; zoomAt(0, 0, GR.t.k / 1.4); });
  ['g-cap', 'g-priv'].forEach(function (id) { $(id).addEventListener('change', function () { GR.sig = ''; buildGraph(); }); });
  $('g-labels').addEventListener('change', function () { drawGraph(); });
  $('g-fit').addEventListener('click', fit);
  window.addEventListener('resize', function () { if (S.view === 'graph') { if (autoFit()) run(); paint(); } });
})();
function legend() {
  var L = $('g-legend');
  L.textContent = '';
  function icon(kind) {
    var s = sv('svg', { width: 14, height: 14, viewBox: '-7 -7 14 14' });
    if (kind === 'tg') s.appendChild(sv('rect', { x: -5, y: -5, width: 10, height: 10, rx: 2, fill: '#c8c8c8' }));
    else if (kind === 'radio') s.appendChild(sv('circle', { r: 4.5, fill: '#c8c8c8' }));
    else if (kind === 'multi') s.appendChild(sv('circle', { r: 4.5, fill: '#7a8288', stroke: '#fff', 'stroke-dasharray': '3 2', 'stroke-width': 1.5 }));
    else s.appendChild(sv('line', { x1: -6, y1: 0, x2: 6, y2: 0, stroke: '#f89406', 'stroke-dasharray': '3 2', 'stroke-width': 2 }));
    return s;
  }
  [['tg', 'talkgroup (size = calls)'], ['radio', 'radio'], ['priv', 'private call'], ['multi', 'on 2+ networks']]
    .forEach(function (x) { L.appendChild(h('span', null, [icon(x[0]), x[1]])); });
  IX.nets.forEach(function (n) { L.appendChild(h('span', null, [sw(n.key), ' ', n.label])); });
}

// ---------- page chrome ----------
function select(type, id) {
  // On a narrow screen the details are a drawer: tapping the selected item
  // again while the drawer is closed opens it rather than deselecting.
  var reopen = drawer() && isSel(type, id) && !document.body.classList.contains('sheet');
  S.sel = isSel(type, id) && !reopen ? null : { type: type, id: id };
  document.body.classList.toggle('sheet', !!S.sel && drawer());
  renderDetail();
  if (S.view === 'graph') highlight(); else renderView();
  if (S.sel && drawer()) $('detail').scrollTop = 0;
}
function closeSheet() {
  document.body.classList.remove('sheet');
  if (!S.sel) return;
  S.sel = null;
  renderDetail();
  if (S.view === 'graph') highlight(); else renderView();
}
// Esc closes the details drawer, or (wide screens) clears the selection --
// unless it is meant for a field, a menu or the audio player.
document.addEventListener('keydown', function (e) {
  if (e.key !== 'Escape') return;
  if (document.body.classList.contains('sheet')) { closeSheet(); return; }
  var t = e.target;
  if (S.sel && !(t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA'))) closeSheet();
});
function setNets(keys) { S.nets = {}; keys.forEach(function (k) { S.nets[k] = 'in'; }); GR.sig = ''; render(); }
// A network chip picks its network, or takes it out of the filter (picked or
// excluded).
function toggleNet(k) { if (S.nets[k]) delete S.nets[k]; else S.nets[k] = 'in'; GR.sig = ''; render(); }
// Exclude a network (or include it again).
function excludeNet(k) { if (S.nets[k] === 'out') delete S.nets[k]; else S.nets[k] = 'out'; GR.sig = ''; render(); }
// Talkgroup / radio filter (name: 'tgf' / 'rf'): Only / All, Exclude /
// Include, dropping one, clearing it.
function setF(name, m) { S[name] = m; GR.sig = ''; render(); }
function onlyIn(name, id) { var ks = keys(S[name]); return ks.length === 1 && ks[0] === id && S[name][id] === 'in'; }
function fOnly(name, id) { var m = {}; if (!onlyIn(name, id)) m[id] = 'in'; setF(name, m); }
function fExclude(name, id) { var m = Object.assign({}, S[name]); if (m[id] === 'out') delete m[id]; else m[id] = 'out'; setF(name, m); }
function fDrop(name, id) { var m = Object.assign({}, S[name]); delete m[id]; setF(name, m); }
// The Only / Exclude buttons in a talkgroup's or radio's details.
function filterBtns(name, id, what, hideTip) {
  var only = onlyIn(name, id), out = S[name][id] === 'out';
  return h('div', { class: 'tagrow' }, [
    h('button', { class: 'btn sm', type: 'button', title: only ? 'Show every ' + what + ' again' : 'Show only this ' + what,
                  onclick: function () { fOnly(name, id); } }, only ? 'All' : 'Only'),
    h('button', { class: 'btn sm', type: 'button', title: out ? 'Show this ' + what + ' again' : hideTip,
                  onclick: function () { fExclude(name, id); } }, out ? 'Include' : 'Exclude')]);
}
function clearSearch() { S.q = ''; $('q').value = ''; GR.sig = ''; render(); }
function clearFilters() { S.nets = {}; S.tgf = {}; S.rf = {}; S.q = ''; $('q').value = ''; GR.sig = ''; render(); }
function onlyNet(k) { var ks = netKeys(); return ks.length === 1 && ks[0] === k && S.nets[k] === 'in'; }
// The network list's collapsed / open state (remembered per browser).
// The Filters section's collapsed / open state (remembered per browser).
function setFOpen(open) { S.fOpen = open; store('filtersOpen', open ? '1' : ''); render(); }
function setView(v) {
  S.view = v; store('view', v);
  var tabs = document.querySelectorAll('#viewtabs .tab');
  for (var i = 0; i < tabs.length; i++) tabs[i].classList.toggle('active', tabs[i].getAttribute('data-v') === v);
  ['calls', 'tgs', 'radios', 'graph', 'map', 'links', 'nets'].forEach(function (x) { $('v-' + x).hidden = x !== v; });
  renderView();
}
function renderView() {
  if (!IX) return;
  if (S.view === 'calls') viewCalls();
  else if (S.view === 'tgs') viewTgs();
  else if (S.view === 'radios') viewRadios();
  else if (S.view === 'nets') viewNets();
  else if (S.view === 'links') viewLinks();
  else if (S.view === 'graph') { legend(); buildGraph(); highlight(); run(); }
  else if (S.view === 'map') viewMap();
}
// The Filters section. Collapsed (the default): one row -- the toggle, a
// chip for each filter in force (click to drop it) and Clear all. Open: a
// row per kind -- every network; the talkgroups and radios in the filter,
// with a box to type ids into (or Only / Exclude in their details); the
// search. The page re-renders on every poll, so the rows (and the boxes in
// them) are built once and only their chips are redrawn: typing isn't lost.
var FUI = null;
// Add the ids typed into a Filters box to talkgroup / radio filter `name`,
// as 'in' (Pick) or 'out' (Exclude). Several ids may be separated by spaces
// or commas; "TG 16" works for a talkgroup, an alias for a radio.
function addTyped(name, text, st) {
  var raw = text.replace(/\bTG\s*(?=\w)/ig, '').split(/[\s,;]+/).filter(Boolean);
  if (!raw.length) return false;
  var m = Object.assign({}, S[name]), unknown = [];
  raw.forEach(function (w) {
    var id = w;
    if (name === 'rf' && !IX.rById[w]) {          // an alias?
      var hit = IX.radios.filter(function (r) { return r.aliases.some(function (a) { return a.toLowerCase() === w.toLowerCase(); }); })[0];
      if (hit) id = hit.id;
    }
    if (!(name === 'tgf' ? IX.tgById[id] : IX.rById[id])) unknown.push(id);
    m[id] = st;
  });
  setF(name, m);
  if (unknown.length) toast((name === 'tgf' ? 'TG ' : 'Radio ') + unknown.join(', ') + (unknown.length > 1 ? ' aren’t' : ' isn’t') +
                            ' in the data yet — added to the filter anyway');
  return true;
}
function filterAdd(name, what, place) {
  var list = h('datalist', { id: 'dl-' + name });
  var inp = h('input', { type: 'text', class: 'fin', placeholder: place, list: 'dl-' + name, autocomplete: 'off',
                         'aria-label': 'Add ' + what + 's to the filter', spellcheck: 'false' });
  var go = function (st) { if (addTyped(name, inp.value, st)) inp.value = ''; inp.focus(); };
  inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); go('in'); } });
  return { list: list, inp: inp, el: h('span', { class: 'fadd' }, [inp, list,
    h('button', { class: 'btn sm', type: 'button', title: 'Show only the picked ' + what + 's (adds to them; Enter does the same)',
                  onclick: function () { go('in'); } }, 'Pick'),
    h('button', { class: 'btn sm', type: 'button', title: 'Hide these ' + what + 's',
                  onclick: function () { go('out'); } }, 'Exclude')]) };
}
function renderFilters() {
  var fp = $('filters');
  if (!FUI) {
    var mk = function (label) { var items = h('span', { class: 'fitems' }); return { el: h('div', { class: 'frow' }, [h('span', { class: 'lbl', text: label }), items]), items: items }; };
    FUI = { head: h('div', { class: 'frow fhead' }), body: h('div', { class: 'fbody' }), net: mk('Networks'), tg: mk('Talkgroups'),
            r: mk('Radios'), q: mk('Search'), tgAdd: filterAdd('tgf', 'talkgroup', 'TG id…'), rAdd: filterAdd('rf', 'radio', 'Radio id or alias…'), dl: '' };
    FUI.tg.el.appendChild(FUI.tgAdd.el); FUI.r.el.appendChild(FUI.rAdd.el);
    [FUI.net, FUI.tg, FUI.r, FUI.q].forEach(function (x) { FUI.body.appendChild(x.el); });
    fp.textContent = '';
    fp.appendChild(FUI.head); fp.appendChild(FUI.body);
  }
  fp.classList.toggle('open', S.fOpen);
  var byCalls = IX.nets.slice().sort(function (a, b) { return b.calls - a.calls; });
  var numSort = function (a, b) { return (+a - +b) || (a < b ? -1 : 1); };
  var tgIds = keys(S.tgf).sort(numSort), rIds = keys(S.rf).sort(numSort);
  var nAct = netKeys().length + tgIds.length + rIds.length + (S.q ? 1 : 0);
  var netChip = function (n) {
    var st = S.nets[n.key];
    return h('span', { class: 'chip' + (st === 'in' ? ' active' : st === 'out' ? ' excl' : ''),
                       title: n.label + ' — ' + n.confidence + ' identity' + (st === 'in' ? ' (click to take it out of the filter)' :
                              st === 'out' ? ' (excluded; click to show it again)' : ' (click to add it to the filter)'),
      onclick: function () { var on = !S.nets[n.key]; toggleNet(n.key); if (on) select('net', n.key); } },
      [sw(n.key), n.label, h('span', { class: 'n', text: String(n.calls) })]);
  };
  var idChip = function (name, id, label) {
    var out = S[name][id] === 'out';
    return h('span', { class: 'chip' + (out ? ' excl' : ' active'),
      title: label + (out ? ' is excluded' : ' is picked') + ' (click to take it out of the filter)',
      onclick: function () { fDrop(name, id); } }, label + ' ✕');
  };
  var tgChip = function (id) { return idChip('tgf', id, 'TG ' + id); };
  var rChip = function (id) { var al = aliasOf(id); return idChip('rf', id, 'Radio ' + id + (al ? ' ' + al : '')); };
  var qChip = function () {
    return h('span', { class: 'chip active', title: 'The search (click to clear it)', onclick: clearSearch }, '“' + S.q + '” ✕');
  };
  var head = FUI.head;
  head.textContent = '';
  head.appendChild(h('button', { class: 'ftog', type: 'button', 'aria-expanded': S.fOpen ? 'true' : 'false',
    title: S.fOpen ? 'Collapse the filters' : 'Show all the filters', onclick: function () { setFOpen(!S.fOpen); } },
    [h('span', { class: 'car', text: '▸' }), 'Filters', nAct ? h('span', { class: 'fcount', text: String(nAct) }) : null]));
  if (!S.fOpen) {
    if (!nAct) head.appendChild(h('span', { class: 'fnone', text: 'none — showing everything' }));
    byCalls.forEach(function (n) { if (S.nets[n.key]) head.appendChild(netChip(n)); });
    tgIds.forEach(function (id) { head.appendChild(tgChip(id)); });
    rIds.forEach(function (id) { head.appendChild(rChip(id)); });
    if (S.q) head.appendChild(qChip());
    head.appendChild(h('span', { class: 'fmore', onclick: function () { setFOpen(true); },
      text: (IX.nets.length ? IX.nets.length + ' network' + (IX.nets.length === 1 ? '' : 's') : 'more') + '…' }));
  }
  if (nAct) head.appendChild(h('a', { class: 'fclear', href: '#', onclick: function (e) { e.preventDefault(); clearFilters(); } }, 'Clear all'));
  FUI.body.hidden = !S.fOpen;
  if (!S.fOpen) return;
  var fill = function (row, items) { row.items.textContent = ''; items.forEach(function (x) { row.items.appendChild(x); }); };
  // Two or more networks picked: they can be merged into one (the busiest).
  var picked = byCalls.filter(function (n) { return S.nets[n.key] === 'in'; });
  fill(FUI.net, [h('span', { class: 'chip' + (netAll() ? ' active' : ''), onclick: function () { setNets([]); } }, 'All networks')]
    .concat(byCalls.map(netChip)).concat(picked.length < 2 ? [] : [h('button', { class: 'btn sm', type: 'button',
      title: 'Show the ' + picked.length + ' picked networks as one -- under ' + picked[0].label + ', the busiest' +
             (S.file ? ' (in this file view only)' : ' (for everyone viewing this server; Unmerge in its details undoes it)'),
      onclick: function () { mergeInto(picked.slice(1).map(function (n) { return n.key; }), picked[0].key); } },
      'Merge picked (' + picked.length + ')')]));
  fill(FUI.tg, [h('span', { class: 'chip' + (tgAll() ? ' active' : ''), onclick: function () { setF('tgf', {}); } }, 'All talkgroups')]
    .concat(tgIds.map(tgChip)));
  fill(FUI.r, [h('span', { class: 'chip' + (fAll(S.rf) ? ' active' : ''), onclick: function () { setF('rf', {}); } }, 'All radios')]
    .concat(rIds.map(rChip)));
  FUI.q.el.hidden = !S.q;
  if (S.q) fill(FUI.q, [qChip()]);
  // Suggestions for the boxes: this protocol's talkgroups and radios (with
  // aliases), rebuilt only when that set changes so an open list stays put.
  var dl = S.fam + '|' + IX.tgs.length + '|' + IX.radios.length;
  if (dl !== FUI.dl) {
    FUI.dl = dl;
    FUI.tgAdd.list.textContent = '';
    IX.tgs.forEach(function (t) { FUI.tgAdd.list.appendChild(h('option', { value: t.id })); });
    FUI.rAdd.list.textContent = '';
    IX.radios.forEach(function (r) {
      FUI.rAdd.list.appendChild(h('option', { value: r.id }, r.aliases.length ? r.aliases[r.aliases.length - 1] : null));
    });
  }
}
function famTotals(f) { var F = S.d.families[f]; return F.calls.length + F.talkgroups.length + F.radios.length; }
// ---- connections ----
// /net.json "streams": the decode streams running now, connected whether or
// not anything is decoded ({s, fam, label, freq, since, heard, live}), and
// "clients": every connected client. A stream "decodes now" when the
// decoder printed anything in the last 10 s.
var HEARD_MS = 10000;
function streamsOf(f) { return S.file || !S.d ? [] : (S.d.streams || []).filter(function (x) { return f == null || x.fam === f; }); }
function decodingNow(x) { return x.heard && now() - x.heard < HEARD_MS; }
function streamText(x) {
  return (FAMN[x.fam] || (x.fam === 'auto' ? 'Detecting protocol' : (x.label || '?').toUpperCase())) +
    (x.freq ? ' ' + mhz(x.freq) + ' MHz' : ' (no frequency)') + ' \u2014 ' +
    (decodingNow(x) ? 'decoding' : x.heard ? 'quiet since ' + ago(x.heard) : 'nothing decoded yet') +
    ' \u00B7 connected ' + ago(x.since).replace(' ago', '');
}
// The row-end summary: "● 4 clients · 6 streams" (green while any stream
// decodes), or "No clients connected". Nothing in a file view.
function connSummary() {
  if (S.file || !S.d || S.d.clients == null) return null;
  var st = streamsOf(null), dec = st.some(decodingNow), n = S.d.clients;
  var tip = n ? n + ' client' + (n > 1 ? 's' : '') + ' connected' + (st.length ? '; decode streams:\n' + st.map(streamText).join('\n') : ', no decode stream running') :
                'No client is connected to this server.';
  return h('span', { class: 'conn' + (n ? ' on' : ''), title: tip, 'data-tip': tip },
    [h('span', { class: 'cdot' + (dec ? ' on' : '') }),
     n ? n + ' client' + (n > 1 ? 's' : '') + (st.length ? ' \u00B7 ' + st.length + ' stream' + (st.length > 1 ? 's' : '') : '') : 'No clients connected']);
}
// A protocol whose streams are connected gets its tab even before any traffic.
function addStreamFams(d) {
  (d.streams || []).forEach(function (x) {
    if (!x.fam || x.fam === 'auto' || d.families[x.fam]) return;
    d.families[x.fam] = { networks: [], talkgroups: [], radios: [], calls: [] };
  });
}
// Rebuilding parts of the page must never move it: if the page's scroll
// position changed while they were rebuilt (a part briefly empty), put it back.
function render() {
  var y = window.scrollY;
  renderAll();
  if (window.scrollY !== y) window.scrollTo(window.scrollX, y);
}
function renderAll() {
  // A protocol tab shows when it has something to show -- a listed network
  // (identified, or with traffic) -- or a decode stream connected to it now
  // (receiving, nothing identified yet). A protocol with only bare
  // "Unidentified · <freq>" buckets and no live stream is dropped with them.
  var fams = Object.keys(S.d.families).filter(function (f) {
    return streamsOf(f).length || S.d.families[f].networks.some(netShown);
  }).sort(function (a, b) { return famTotals(b) - famTotals(a); });
  $('empty').hidden = fams.length > 0;
  $('main').hidden = !fams.length;
  var ft = $('famtabs');
  ft.textContent = '';
  var conn = connSummary(), imps = impChip();
  if (!fams.length) { IX = null; if (imps) ft.appendChild(imps); if (conn) ft.appendChild(conn); return; }
  if (fams.indexOf(S.fam) < 0) { S.fam = fams[0]; S.nets = {}; S.tgf = {}; S.rf = {}; S.sel = null; }
  fams.forEach(function (f) {
    var F = S.d.families[f], lv = F.calls.filter(live).length, st = streamsOf(f), dec = st.some(decodingNow);
    ft.appendChild(h('button', { class: 'tab' + (f === S.fam ? ' active' : ''), type: 'button',
      title: F.radios.length + ' radios' + (lv ? ', ' + lv + ' live calls' : '') + ' on all of its networks' +
             (st.length ? '\n' + st.length + ' stream' + (st.length > 1 ? 's' : '') + ' connected' + (dec ? ' (decoding)' : ' (quiet)') + ':\n' +
                          st.map(streamText).join('\n') : ''),
      onclick: function () { S.fam = f; store('fam', f); S.nets = {}; S.tgf = {}; S.rf = {}; S.sel = null; GR.sig = ''; render(); } },
      [st.length ? h('span', { class: 'cdot' + (dec ? ' on' : '') }) : null,
       FAMN[f] || f.toUpperCase(), ' ', h('span', { class: 'count', text: F.radios.length + ' radios' + (lv ? ' · ' + lv + ' live' : '') })]));
  });
  if (imps) ft.appendChild(imps);
  if (conn) ft.appendChild(conn);
  index();
  netKeys().forEach(function (k) { if (!IX.netByKey[k]) delete S.nets[k]; });
  var calls = fCalls(null, true), tgs = fTgs(), radios = fRadios(), lv = calls.filter(live).length, sites = {};
  IX.nets.forEach(function (n) { if (netOk(n.key)) n.sites.forEach(function (s) { sites[n.key + s] = 1; }); });
  var cards = $('cards');
  cards.textContent = '';
  var kept = 'The newest calls are listed (up to ' + (S.d.max_calls || 5000) + ' per protocol; calls with audio are kept longest).';
  // Live: a Streams card (this protocol's connected decode streams; green
  // when one decodes now). A file view has no connections: no Streams card.
  var st = streamsOf(S.fam), stDec = st.some(decodingNow);
  var cardStreams = S.file ? null :
    ['Streams', st.length, st.length ? st.length + ' decode stream' + (st.length > 1 ? 's' : '') + ' connected for this protocol:\n' + st.map(streamText).join('\n')
                                      : 'No decode stream connected for this protocol right now (its data is from earlier, or imported).'];
  [['Networks', IX.nets.filter(function (n) { return netOk(n.key); }).length], ['Sites', Object.keys(sites).length], ['Talkgroups', tgs.length],
   ['Radios', radios.length], ['Calls (recent)', calls.length, kept], cardStreams,
   ['Live calls', lv, !anyFilter() && !S.q ? 'Calls heard in the last ' + LIVE_MS / 1000 + ' s.' :
     'Calls heard in the last ' + LIVE_MS / 1000 + ' s' + (anyFilter() ? ' with the filters' : '') +
     (S.q ? ' matching the search' : '') + ' (the protocol tab counts all of them).']].filter(Boolean).forEach(function (c, i, all) {
    if (!i) cards.style.setProperty('--ncards', all.length);   // one row on tablets (a file view has no Streams card)
    var isLive = c[0] === 'Live calls', isSt = c === cardStreams;
    cards.appendChild(h('div', { class: 'card' + ((isLive && lv) || (isSt && stDec) ? ' live' : ''), title: c[2] || null, 'data-tip': c[2] || null },
      [h('div', { class: 'n' }, [isSt && st.length ? h('span', { class: 'cdot' + (stDec ? ' on' : '') }) : null, String(c[1])]),
       h('div', { class: 'l', text: c[0] })]));
  });
  renderFilters();
  $('c-calls').textContent = S.audOnly && (!S.file || !fAll(FILEAUDIO)) ? fCalls().length : calls.length;   // the list's own rows
  $('c-tgs').textContent = tgs.length;
  $('c-radios').textContent = radios.length; $('c-nets').textContent = IX.nets.length;
  renderView();
  renderDetail();
}

// ---------- export / open ----------
$('exp').addEventListener('click', function (e) { e.stopPropagation(); $('expmenu').hidden = !$('expmenu').hidden; });
$('exp-audio').addEventListener('click', function (e) { e.preventDefault(); $('expmenu').hidden = true; exportWithAudio(); });
$('exp-kml').addEventListener('click', function (e) {
  e.preventDefault();
  $('expmenu').hidden = true;
  var rs = IX ? fRadios().filter(function (r) { return trackOf(r).length; }) : [];
  if (!rs.length) { toast('No position reports among the listed radios.'); return; }
  saveText('dsd_positions_' + (S.fam || '') + '_' + stamp(now()) + '.kml', tracksKml(rs, 'dsd-server ' + (FAMN[S.fam] || S.fam) + ' positions'),
           'application/vnd.google-earth.kml+xml');
  toast('Saved the positions of ' + rs.length + ' radio' + (rs.length > 1 ? 's' : '') + '.');
});
document.addEventListener('click', function () { $('expmenu').hidden = true; });
// Compact header (narrow screens): the actions live in a menu.
function setMenu(open) { $('acts').classList.toggle('open', open); $('more').setAttribute('aria-expanded', open ? 'true' : 'false'); }
$('more').addEventListener('click', function (e) { e.stopPropagation(); setMenu(!$('acts').classList.contains('open')); });
$('acts').addEventListener('click', function (e) {
  if (e.target.closest('#exp')) return;                  // the Export submenu stays open in the menu
  if (e.target.closest('.btn, .menu a')) setMenu(false);
});
document.addEventListener('click', function (e) { if (!e.target.closest('#acts')) setMenu(false); });
function srcText(s) {
  function t(ms) { return ms ? dt(ms).slice(5, 16) : 'start'; }
  return (s.name || (s.instance || '').slice(0, 8) || '?') + ' ' + t(s.since) + '–' + t(s.through).slice(6) + 'Z';
}
function reportList(rep) {
  return h('ul', { class: 'report' }, rep.map(function (r) {
    var cls = /^(merged|imported|replaced)$/.test(r.status) ? 'ok' : r.status === 'skipped' ? 'skip' : 'bad';
    return h('li', null, [h('span', { class: 'st ' + cls, text: r.status }), h('b', { text: r.name }),
      r.message && r.message !== r.status ? ' — ' + r.message : '']);
  }));
}
function openText(text, name) {
  if (/^\s*\{"op":"header"/.test(text)) {
    alert('"' + name + '" is a recording (the raw decoder input), not an export.\n\n' +
          'Turn it into an export with:\n  net-replay ' + name + ' --export out.json\nthen open out.json here.');
    return;
  }
  var d;
  try { d = JSON.parse(text); } catch (e) { alert('"' + name + '" is not valid JSON.'); return; }
  openData(d, name);
}
// Show an export (or a merge of several) read-only. `report` = what happened
// to each file of a merge.
function openData(d, name, report) {
  var ok = d && typeof d.families === 'object' && (d.format === 'dsd-net-export' || typeof d.now === 'number');
  if (!ok) { alert('"' + name + '" is not a dsd-server explorer export (expected format "dsd-net-export").'); return; }
  if (d.format === 'dsd-net-export' && d.format_version > 1)
    alert('This export uses a newer format (v' + d.format_version + '); some details may not show.');
  S.file = { name: name, exported: d.exported || d.now, source: d.name || d.source || '' };
  S.d = { version: -Date.now(), now: d.now || d.exported, families: d.families, rec: {}, merges: d.merges || {} };
  applyMerges(S.d);
  S.fam = null; S.nets = {}; S.tgf = {}; S.rf = {}; S.sel = null; S.q = ''; $('q').value = ''; GR.sig = ''; GR.fitted = false; GR.userView = false;
  document.body.classList.add('filemode');
  var fb = $('filebar'), srcs = d.sources || [];
  fb.textContent = '';
  fb.appendChild(h('span', null, [report ? 'Viewing a merge of ' : 'Viewing export ', h('b', { text: name })]));
  fb.appendChild(h('span', { class: 'm', text: (report ? 'merged ' : 'exported ') + dt(S.file.exported) +
    (!report && S.file.source ? ' by ' + S.file.source : '') + ' · times are relative to the export' }));
  if (srcs.length > 1 || report)
    fb.appendChild(h('span', { class: 'm', text: 'sources: ' + srcs.map(srcText).join(', ') }));
  if (report) {
    var blob = new Blob([JSON.stringify(d)], { type: 'application/json' });
    fb.appendChild(h('a', { href: URL.createObjectURL(blob), download: 'net_merged.json' }, 'Save merged (.json)'));
  }
  fb.appendChild(h('a', { href: '#', onclick: function (e) { e.preventDefault(); backToLive(); } }, 'Back to live'));
  if (report) fb.appendChild(reportList(report));
  fb.hidden = false;
  $('live').textContent = 'file view';
  render();
}
// ---- "export with audio" zips ----
// The entries of a zip: [{name, data}] -- stored ones as they are, deflated
// ones through the browser's decompressor (a zip re-packed by another tool).
function unzip(buf) {
  var v = new DataView(buf), u8 = new Uint8Array(buf), dec = new TextDecoder(), e = -1;
  for (var i = buf.byteLength - 22; i >= Math.max(0, buf.byteLength - 66000); i--) if (v.getUint32(i, true) === 0x06054b50) { e = i; break; }
  if (e < 0) return Promise.reject(new Error('not a zip file'));
  var n = v.getUint16(e + 10, true), p = v.getUint32(e + 16, true), out = [];
  for (var k = 0; k < n; k++) {
    if (v.getUint32(p, true) !== 0x02014b50) return Promise.reject(new Error('damaged zip'));
    var meth = v.getUint16(p + 10, true), csz = v.getUint32(p + 20, true), nl = v.getUint16(p + 28, true),
        xl = v.getUint16(p + 30, true), cl = v.getUint16(p + 32, true), lo = v.getUint32(p + 42, true);
    var name = dec.decode(u8.subarray(p + 46, p + 46 + nl));
    var ds = lo + 30 + v.getUint16(lo + 26, true) + v.getUint16(lo + 28, true);
    out.push({ name: name, meth: meth, data: u8.subarray(ds, ds + csz) });
    p += 46 + nl + xl + cl;
  }
  return Promise.all(out.filter(function (x) { return !/\/$/.test(x.name); }).map(function (x) {
    if (x.meth === 0) return { name: x.name, data: x.data };
    if (x.meth === 8 && typeof DecompressionStream !== 'undefined')
      return new Response(new Blob([x.data]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer()
        .then(function (b) { return { name: x.name, data: new Uint8Array(b) }; });
    return null;
  })).then(function (l) { return l.filter(Boolean); });
}
// A zip's export (its .json) -- and, with `audio`, its .wav files registered
// for playback in the file view.
function readZip(f, audio) {
  return f.arrayBuffer().then(unzip).then(function (ents) {
    var js = ents.filter(function (x) { return /\.json$/i.test(x.name); })[0];
    if (!js) throw new Error('no explorer export (.json) in it');
    if (audio) ents.forEach(function (x) {
      if (/\.wav$/i.test(x.name)) FILEAUDIO[x.name.replace(/^.*\//, '')] = URL.createObjectURL(new Blob([x.data], { type: 'audio/wav' }));
    });
    return new TextDecoder().decode(js.data);
  });
}
var EXPORT_AUDIO_MAX = 1024 * 1048576;          // the zip stops adding audio here (it is built in the browser)
function exportWithAudio() {
  var calls = [], files = [], total = 0, i = 0, missing = 0, full = false, seen = {};
  toast('Making the export\u2026');
  fetch('/net/export.json', { cache: 'no-store' }).then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); })
  .then(function (txt) {
    var d = JSON.parse(txt);
    for (var f in d.families) (d.families[f].calls || []).forEach(function (c) { if (c.audio && !seen[c.audio]) { seen[c.audio] = 1; calls.push(c); } });
    function next() {
      if (i >= calls.length || full) return Promise.resolve();
      var c = calls[i++];
      if (i % 20 === 0) toast('Adding audio ' + i + ' / ' + calls.length + '\u2026');
      return fetch('/net/audio/' + encodeURIComponent(c.audio)).then(function (r) { if (!r.ok) throw new Error(); return r.arrayBuffer(); })
        .then(function (b) {
          if (total + b.byteLength > EXPORT_AUDIO_MAX) { full = true; return; }
          total += b.byteLength;
          files.push({ name: 'audio/' + c.audio, data: new Uint8Array(b) });
        }, function () { ++missing; }).then(next);
    }
    return next().then(function () {
      var name = 'dsd_net_export_' + stamp(now());
      files.unshift({ name: name + '.json', data: new TextEncoder().encode(txt) });
      var a = h('a', { href: URL.createObjectURL(zipBlob(files)), download: name + '.zip' });
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(function () { URL.revokeObjectURL(a.href); }, 60000);
      var n = files.length - 1;
      toast('Saved the explorer data with ' + n + ' call' + (n === 1 ? '\u2019s' : 's\u2019') + ' audio (' + mb(total) + ')' +
            (full ? ' \u2014 the zip stops at ' + mb(EXPORT_AUDIO_MAX) + ', so ' + (calls.length - n - missing) + ' more are left out' : '') +
            (missing ? '; ' + missing + ' no longer on the server' : '') + '.');
    });
  }).catch(function (e) { toast('Could not make the export (' + e.message + ').'); });
}
function readFile(f) {
  if (/\.zip$/i.test(f.name)) return readZip(f, true);
  var gz = /\.gz$/i.test(f.name) && typeof DecompressionStream !== 'undefined';
  return gz ? new Response(f.stream().pipeThrough(new DecompressionStream('gzip'))).text() : f.text();
}
function openFile(f) {
  if (!f) return;
  clearFileAudio();
  readFile(f).then(function (t) { openText(t, f.name); }).catch(function () { alert('Could not read "' + f.name + '".'); });
}
// Several files: merged by the server (the same merge as Import and
// net-merge), without touching the live data.
function openFiles(fl) {
  var files = Array.prototype.slice.call(fl || []);
  if (files.length < 2) { openFile(files[0]); return; }
  clearFileAudio();
  Promise.all(files.map(function (f) {
    return readFile(f).then(function (t) { return { name: f.name, text: t }; });
  })).then(function (list) {
    return fetch('/net/merge', { method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' },
                                 body: JSON.stringify({ files: list }) });
  }).then(function (r) { return r.json(); }).then(function (res) {
    if (res.error) { alert('Merge failed: ' + res.error); return; }
    var n = res.report.filter(function (r) { return /^(merged|replaced)$/.test(r.status); }).length;
    openData(res.export, files.length + ' files', res.report);
    if (!n) alert('None of the files could be merged:\n\n' + res.report.map(function (r) { return r.name + ': ' + r.message; }).join('\n'));
  }).catch(function (e) { alert('Could not merge the files (' + e + ').'); });
}
function backToLive() {
  if (PLAYER.a && PLAYER.name && S.file) { PLAYER.a.pause(); PLAYER.name = null; }
  clearFileAudio();
  S.file = null; S.d = null; IX = null; S.fam = load('fam'); S.nets = {}; S.tgf = {}; S.rf = {}; S.sel = null; GR.sig = ''; GR.fitted = false; GR.userView = false;
  document.body.classList.remove('filemode');
  $('filebar').hidden = true;
  $('live').textContent = 'connecting\u2026';
}
$('open').addEventListener('click', function () { $('openfile').value = ''; $('openfile').click(); });
$('openfile').addEventListener('change', function () { openFiles(this.files); });
document.addEventListener('dragover', function (e) { e.preventDefault(); document.body.classList.add('dragging'); });
document.addEventListener('dragleave', function (e) { if (!e.relatedTarget) document.body.classList.remove('dragging'); });
document.addEventListener('drop', function (e) {
  e.preventDefault(); document.body.classList.remove('dragging');
  if (e.dataTransfer && e.dataTransfer.files.length) openFiles(e.dataTransfer.files);
});

// ---------- import (exports added to the live view) ----------
// Each file is posted as is (JSON or gzip); the server decides whether it
// can join without counting anything twice and says why not.
function importFiles(fl) {
  var files = Array.prototype.slice.call(fl || []), out = [];
  var note = $('impnote');
  note.textContent = '';
  note.appendChild(h('span', { class: 'm', text: 'Importing ' + files.length + ' file' + (files.length > 1 ? 's' : '') + '…' }));
  note.hidden = false;
  files.reduce(function (p, f) {
    return p.then(function () {
      var body = /\.zip$/i.test(f.name) ? readZip(f, false) : Promise.resolve(f);
      return body.then(function (b) { return fetch('/net/import?name=' + encodeURIComponent(f.name), { method: 'POST', cache: 'no-store', body: b }); })
        .then(function (r) { return r.json(); })
        .then(function (r) { out.push(r); }, function (e) { out.push({ name: f.name, status: 'failed', message: String(e) }); });
    });
  }, Promise.resolve()).then(function () {
    note.textContent = '';
    note.appendChild(h('span', null, h('b', { text: 'Import' })));
    note.appendChild(h('span', { class: 'x', title: 'Dismiss', onclick: function () { note.hidden = true; } }, '✕'));
    note.appendChild(reportList(out));
    S.impsig = null;
  });
}
// The bar can be hidden (its ✕): remembered for this set of imports, so a new
// or removed import brings it back. While hidden, a small "N imports" beside
// the protocol tabs says they are still included, and brings the bar back.
function impIds(list) { return (list || []).map(function (x) { return x.id; }).join(','); }
function impHidden() { var l = S.d && S.d.imports; return !!(l && l.length) && load('imphide') === impIds(l); }
function showImports(hide) {
  store('imphide', hide ? impIds(S.d && S.d.imports) : '');
  S.impsig = null;
  updateImports(S.d && S.d.imports);
  if (IX) render();
}
function impChip() {
  if (S.file || !impHidden()) return null;
  var n = S.d.imports.length, tip = 'Data from ' + n + ' imported export' + (n > 1 ? 's is' : ' is') + ' included — show the imports bar';
  return h('a', { class: 'imps', href: '#', title: tip, 'data-tip': tip,
                  onclick: function (e) { e.preventDefault(); showImports(false); } }, n + ' import' + (n > 1 ? 's' : ''));
}
function updateImports(list) {
  list = list || [];
  var sig = JSON.stringify(list.map(function (x) { return [x.id, x.calls]; }));
  if (sig === S.impsig) return;
  S.impsig = sig;
  var bar = $('impbar');
  bar.textContent = '';
  bar.hidden = !list.length || load('imphide') === impIds(list);
  if (!list.length) return;
  bar.appendChild(h('span', null, ['Including ', h('b', { text: list.length + ' import' + (list.length > 1 ? 's' : '') }), ':']));
  list.forEach(function (x) {
    var tip = x.networks + ' networks · ' + x.talkgroups + ' talkgroups · ' +
        x.radios + ' radios · ' + x.calls + ' calls\n' + (x.sources || []).map(srcText).join('\n');
    bar.appendChild(h('span', { class: 'imp-item', title: tip, 'data-tip': tip }, [
      x.label, h('span', { class: 'm', text: ' (' + (x.sources || []).map(function (s) { return s.name || '?'; }).join(', ') + ')' }),
      h('a', { href: '#', title: 'Remove this import', onclick: function (e) {
        e.preventDefault(); fetch('/net/imports/remove?id=' + x.id, { cache: 'no-store' }).then(function () { S.impsig = null; });
      } }, '✕')]));
  });
  bar.appendChild(h('a', { href: '#', onclick: function (e) {
    e.preventDefault(); fetch('/net/imports/clear', { cache: 'no-store' }).then(function () { S.impsig = null; });
  } }, 'Remove all'));
  bar.appendChild(h('span', { class: 'x', title: 'Hide this bar (the imports stay included; "' + list.length + ' import' +
                              (list.length > 1 ? 's' : '') + '" beside the protocol tabs brings it back)',
                              onclick: function () { showImports(true); } }, '✕'));
}
$('import').addEventListener('click', function () { $('importfile').value = ''; $('importfile').click(); });
$('importfile').addEventListener('change', function () { importFiles(this.files); });

// ---------- recording (Record button) ----------
function mb(n) { return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB'; }
// Developer tools (Record and its status): hidden from regular users. One
// browser opts in with /net?dev=1 (remembered; ?dev=0 hides them again); a
// server started with DSD_NET_DEV=1 shows them to everyone.
(function () {
  var m = /[?&]dev=([01])\b/.exec(location.search);
  if (m) store('dev', m[1] === '1' ? '1' : '');
})();
function updateDev() { document.body.classList.toggle('devtools', load('dev') === '1' || !!(S.d && S.d.dev)); }
updateDev();
function updateRec(r) {
  S.rec = r || {};
  var b = $('rec'), st = $('recst');
  b.textContent = S.rec.on ? '\u25A0 Stop recording' : '\u25CF Record';
  b.classList.toggle('recon', !!S.rec.on);
  st.textContent = '';
  if (!S.rec.file) return;
  st.title = S.rec.path || '';
  var dl = h('a', { href: '/net/log/download', download: S.rec.file }, 'Download');
  if (S.rec.on) st.appendChild(h('span', null, [h('span', { class: 'live', text: 'REC ' }),
    S.rec.file + ' \u00B7 ' + mb(S.rec.file_bytes || 0) + (S.rec.truncated ? ' \u00B7 size cap reached' : '') + ' \u00B7 ', dl]));
  else st.appendChild(h('span', null, ['Last recording: ' + S.rec.file + ' \u00B7 ', dl]));
}
$('rec').addEventListener('click', function () {
  var url;
  if (S.rec && S.rec.on) {
    url = '/net/log/off';
  } else {
    var hasData = S.d && Object.keys(S.d.families).length > 0;
    var clear = hasData && confirm('Clear the explorer first so the recording replays exactly?\n\n' +
      'OK = clear, then record (recommended)\nCancel = record on top of the current data');
    url = '/net/log/on' + (clear ? '?clear=1' : '');
  }
  fetch(url, { cache: 'no-store' }).then(function (r) {
    if (!r.ok) alert('Could not start recording \u2014 check that DSD_NET_LOG_DIR (or DSD_IQ_LOG_DIR) is writable.');
    return r.json();
  }).then(updateRec).catch(function () {});
});

// ---------- per-call audio switch ----------
function updateAudio(a) {
  S.audio = a || {};
  var b = $('aud');
  b.classList.toggle('on', !!S.audio.on);
  b.textContent = S.audio.on ? '\u266B Audio on' : '\u266B Audio';
  b.title = S.audio.on
    ? 'Recording each call\'s voice into ' + S.audio.dir + ' (' + mb(S.audio.bytes || 0) + ' of ' + mb(S.audio.cap_bytes || 0) +
      ', ' + (S.audio.files || 0) + ' files). Click to stop.'
    : 'Record each call\'s decoded voice, to play back here (off by default; encrypted calls are never recorded)';
}
$('aud').addEventListener('click', function () {
  fetch(S.audio && S.audio.on ? '/net/audio/off' : '/net/audio/on', { cache: 'no-store' }).then(function (r) {
    if (!r.ok) alert('Could not start recording audio \u2014 check that DSD_NET_AUDIO_DIR (or DSD_NET_LOG_DIR) is writable.');
    return r.json();
  }).then(updateAudio).catch(function () {});
});

// An update rebuilds the lists and the details, which would cut short what
// the user is doing: a click (the button is replaced between press and
// release, so the click is lost), a drag, a text selection (to copy an id),
// a scroll (momentum on a touch screen stops). While one of those is under
// way the update waits -- checked again shortly, and the status line says
// why. (The details' Merge into... list and a phone's Sort menu guard
// themselves: mergePickBusy, poll.)
var HOLD = { down: 0, scroll: 0 };
document.addEventListener('pointerdown', function (e) {
  if (e.target.closest && e.target.closest('#main')) HOLD.down = Date.now();
}, true);
['pointerup', 'pointercancel', 'dragend'].forEach(function (ev) {
  document.addEventListener(ev, function () { HOLD.down = 0; }, true);
});
window.addEventListener('blur', function () { HOLD.down = 0; });
document.addEventListener('scroll', function (e) {
  var t = e.target;
  if (t === document || (t.closest && t.closest('#main'))) HOLD.scroll = Date.now();
}, true);
function holdReason() {
  var t = Date.now();
  if (HOLD.down && t - HOLD.down < 30000) return ['clicking', 'while you click or drag'];
  if (t - HOLD.scroll < 500) return ['scrolling', 'while you scroll'];
  var s = window.getSelection && getSelection();
  if (s && !s.isCollapsed && s.rangeCount) {
    var n = s.getRangeAt(0).commonAncestorContainer;
    if (n && n.nodeType !== 1) n = n.parentNode;
    if (n && n.closest && n.closest('#main')) return ['text selected', 'while text is selected (click anywhere to clear the selection)'];
  }
  return null;
}
// ---- a new UI on the server ----
// The server stamps the page with its UI build and sends the build it serves
// now with every poll (/net.json "ui"). When they differ the server was
// updated: the page reloads itself -- keeping the protocol, filters, search
// and selection (sessionStorage) -- once the user isn't in the middle of
// something (playing audio, transcribing, clicking, typing).
var UI_BUILD = '%%UI_BUILD%%';
var UINEW = { ui: '', told: false };
function uiBusy() {
  if (holdReason() || mergePickBusy()) return true;
  if (PLAYER.a && !PLAYER.a.paused) return true;
  if (ASR.job) return true;
  var ae = document.activeElement;
  if (ae && (ae.tagName === 'SELECT' || ae.tagName === 'TEXTAREA' ||
             (ae.tagName === 'INPUT' && /^(text|search)$/.test(ae.type) && ae.value))) return true;
  return !!(FUI && (FUI.tgAdd.inp.value || FUI.rAdd.inp.value));
}
function uiCheck(ui) {
  if (!ui || UI_BUILD.charAt(0) === '%' || ui === UI_BUILD) return;
  var done = null;
  try { done = sessionStorage.getItem('netx.reloadedFor'); } catch (e) {}
  if (done === ui) {                     // reloaded for it once already and still old: a cache in the way
    if (!UINEW.told) { UINEW.told = true; toast('The explorer was updated on the server \u2014 refresh the page (Ctrl+F5) to load it.'); }
    return;
  }
  if (uiBusy()) {
    if (!UINEW.told) { UINEW.told = true; toast('The explorer was updated on the server \u2014 it will reload when you\u2019re done.'); }
    return;
  }
  try {
    sessionStorage.setItem('netx.reloadedFor', ui);
    sessionStorage.setItem('netx.reload', JSON.stringify({ fam: S.fam, nets: S.nets, tgf: S.tgf, rf: S.rf, sel: S.sel, q: S.q,
                                                           y: window.scrollY, dy: $('detail').scrollTop }));
  } catch (e) {}
  location.reload();
}
// After such a reload: back to where the user was.
function uiRestore() {
  var r = null;
  try { r = JSON.parse(sessionStorage.getItem('netx.reload') || 'null'); sessionStorage.removeItem('netx.reload'); } catch (e) {}
  if (!r) return;
  if (r.fam) S.fam = r.fam;
  S.nets = r.nets || {}; S.tgf = r.tgf || {}; S.rf = r.rf || {}; S.sel = r.sel || null;
  S.q = r.q || ''; $('q').value = S.q;
  S.restoreY = { y: r.y || 0, dy: r.dy || 0 };   // applied once the first data is drawn (poll)
}
function poll() {
  if (S.paused || S.file || document.hidden) { setTimeout(poll, POLL); return; }
  var held = S.d && holdReason();
  if (held) {
    $('live').textContent = 'live · held: ' + held[0];
    $('live').title = 'The page waits to update ' + held[1] + ', so it isn\u2019t pulled away from under you.';
    setTimeout(poll, 250);
    return;
  }
  $('live').title = '';
  fetch('/net.json', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (d) {
    uiCheck(d.ui);
    if (holdReason()) return;                          // started while this was on its way: it waits for the next one
    S.skew = d.now - Date.now();
    updateRec(d.rec);
    updateAudio(d.audio);
    updateImports(d.imports);
    var changed = !S.d || d.version !== S.d.version || (d.clients !== S.d.clients) ||
                  JSON.stringify((d.streams || []).map(function (x) { return [x.s, x.fam, decodingNow(x)]; })) !==
                  JSON.stringify((S.d.streams || []).map(function (x) { return [x.s, x.fam, decodingNow(x)]; }));
    addStreamFams(d);
    applyMerges(d);
    S.d = d;
    updateDev();
    $('live').textContent = 'live · updated ' + hms(d.now) + 'Z';
    // A phone's Sort menu open: rebuilding the list would close it -- this
    // update waits for the next poll. (The details panel guards its own
    // picker: mergePickBusy.)
    var ae = document.activeElement;
    if (ae && ae.tagName === 'SELECT' && ae.closest('.sortbar')) { if (changed) S.d.version = -1; }
    else if (changed || S.view === 'calls') render();
    else { renderDetail(); }
    if (S.restoreY) {                                  // after a self-reload: back to where the page was scrolled
      $('detail').scrollTop = S.restoreY.dy; renderDetail.who = S.sel ? S.sel.type + ':' + S.sel.id : '';
      window.scrollTo(window.scrollX, S.restoreY.y);
      S.restoreY = null;
    }
  }).catch(function () { $('live').textContent = 'disconnected — retrying'; })
    .then(function () { setTimeout(poll, POLL); });
}
document.querySelectorAll('#viewtabs .tab').forEach(function (t) {
  t.addEventListener('click', function () { setView(t.getAttribute('data-v')); });
});
var qt = 0;
$('q').addEventListener('input', function (e) {
  clearTimeout(qt);
  qt = setTimeout(function () { S.q = e.target.value.trim(); GR.sig = ''; if (S.d) render(); }, 200);
});
$('pause').addEventListener('click', function () {
  S.paused = !S.paused;
  this.textContent = S.paused ? 'Resume' : 'Pause';
  this.classList.toggle('on', S.paused);
  $('live').textContent = S.paused ? 'paused — view frozen' : 'live';
});
$('clear').addEventListener('click', function () {
  if (!confirm('Forget all calls, talkgroups, radios and networks learned so far' +
               (S.d && S.d.imports && S.d.imports.length ? ', and the imports' : '') + '?')) return;
  // The call audio files stay unless asked: they are on the server's disk.
  var au = S.d && S.d.audio, audio = false;
  if (au && au.files > 0)
    audio = confirm('Also delete the ' + au.files + ' call audio file' + (au.files === 1 ? '' : 's') + ' (' + mb(au.bytes) +
                    ') from the server?\n\nOK deletes them \u2014 they can\u2019t be recovered. Cancel keeps them (still playable from exports and downloads made earlier).');
  if (audio && PLAYER.a && PLAYER.name) { PLAYER.a.pause(); PLAYER.name = null; syncPlay(); }
  fetch('/net/clear' + (audio ? '?audio=1' : ''), { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (r) {
    S.sel = null; S.nets = {}; S.tgf = {}; S.rf = {}; GR.sig = '';
    toast('Cleared.' + (audio ? ' Deleted ' + r.audio_files + ' audio file' + (r.audio_files === 1 ? '' : 's') + ' (' + mb(r.audio_bytes) + ').' : ''));
  }, function () { toast('Could not clear (is the server reachable?).'); });
});
['(max-width: 640px)', '(max-width: 1050px)'].forEach(function (q) {
  if (!window.matchMedia) return;
  var m = window.matchMedia(q), f = function () {
    if (!drawer()) document.body.classList.remove('sheet');
    else if (S.sel) document.body.classList.add('sheet');
    if (S.d) render();
  };
  if (m.addEventListener) m.addEventListener('change', f); else if (m.addListener) m.addListener(f);
});
// Calls toolbar and the now-playing bar.
S.audOnly = load('audonly') === '1';
S.fOpen = load('filtersOpen') === '1';
$('audonly').checked = S.audOnly;
$('audonly').addEventListener('change', function () { S.audOnly = this.checked; store('audonly', S.audOnly ? '1' : '0'); if (S.d) render(); });
$('asron').checked = ASR.on;
$('asron').addEventListener('change', function () {
  ASR.on = this.checked; store('asr', ASR.on ? '1' : '0');
  if (ASR.on && PLAYER.call) transcribe(PLAYER.call); else npText();
});
$('asrlang').addEventListener('change', function () {
  ASR.lang = this.value; store('asr.lang', ASR.lang);
  if (ASR.on && PLAYER.call) transcribe(PLAYER.call);
});
$('asrmodel').addEventListener('change', function () {
  ASR.model = this.value; store('asr.model', ASR.model);
  $('asrlang').hidden = /\.en$/.test(ASR.model);
  if (ASR.on && PLAYER.call) transcribe(PLAYER.call);
});
$('zip').addEventListener('click', zipCalls);
$('npplay').addEventListener('click', function () { if (PLAYER.call) playAudio(PLAYER.call); });
$('npx').addEventListener('click', closeNp);
asrCfg().catch(function () {});
S.fam = load('fam');
uiRestore();
setView(load('view') || 'calls');
poll();
})();
</script>
</body>
</html>
)NETPAGE";
}

// The explorer's speech-to-text worker (GET /net/asr_worker.js): runs Whisper
// (Transformers.js, ONNX Runtime WebAssembly) off the page's main thread.
// Messages in: {cmd:'load', lib, wasm, local, model} then {cmd:'run', id,
// pcm (16 kHz mono Float32Array), language}. Out: progress / ready / result /
// error. The library and model come from the server's /net/asr/ folder, or
// -- only if the user chose to -- from the internet (jsDelivr, Hugging Face).
inline std::string render_asr_worker_js() {
    return R"ASRW('use strict';
let T = null, asr = null;
self.onmessage = async (e) => {
  const m = e.data;
  try {
    if (m.cmd === 'load') {
      T = await import(m.lib);
      const env = T.env;
      env.allowLocalModels = !!m.local;
      env.allowRemoteModels = !m.local;
      if (m.local) env.localModelPath = m.local;
      env.useBrowserCache = typeof caches !== 'undefined';
      if (m.wasm) env.backends.onnx.wasm.wasmPaths = m.wasm;
      const threads = self.crossOriginIsolated ? Math.max(1, Math.min(8, navigator.hardwareConcurrency || 4)) : 1;
      env.backends.onnx.wasm.numThreads = threads;
      const files = {};
      const t0 = performance.now();
      asr = await T.pipeline('automatic-speech-recognition', m.model, {
        dtype: 'q8', device: 'wasm',
        progress_callback: (p) => {
          if (p.status !== 'progress' || !p.total) return;
          files[p.file] = [p.loaded, p.total];
          let a = 0, b = 0;
          for (const k in files) { a += files[k][0]; b += files[k][1]; }
          postMessage({ type: 'progress', loaded: a, total: b });
        } });
      postMessage({ type: 'ready', model: m.model, threads: threads, ms: Math.round(performance.now() - t0) });
    } else if (m.cmd === 'run') {
      const t0 = performance.now();
      // At most ~8 tokens per second of audio: speech never needs more, and it
      // stops Whisper's loops ("I'm telling you, I'm telling you, ...") from
      // running to the 30 s window's limit, which can take half a minute.
      const opts = { chunk_length_s: 30, stride_length_s: 5, return_timestamps: false,
                     max_new_tokens: Math.min(440, Math.ceil(m.pcm.length / 16000 * 8) + 16) };
      if (m.language) { opts.language = m.language; opts.task = 'transcribe'; }
      const r = await asr(m.pcm, opts);
      postMessage({ type: 'result', id: m.id, text: String((r && r.text) || '').trim(), ms: Math.round(performance.now() - t0) });
    }
  } catch (err) {
    postMessage({ type: 'error', id: m.id, cmd: m.cmd, msg: String((err && err.message) || err) });
  }
};
)ASRW";
}

} // namespace dsdsrv
