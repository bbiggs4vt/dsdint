// net_page.hpp
//
// The network explorer page (GET /net): a live, self-contained (no CDN) view
// of the AssocModel (assoc_model.hpp) -- calls, talkgroups, radios, networks,
// a force-directed association graph, and a "links" analysis of radios and
// talkgroups tied together through shared talkgroups / private calls or seen
// on more than one network. Everything is per protocol family.
//
// The page is static; its script polls /net.json and renders client-side.
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
  .hdr-actions { display: flex; gap: .5rem; align-items: center; flex-wrap: wrap; }
  input[type=search] { background: #1f2327; color: var(--heading); border: 1px solid var(--comp-bd);
                       border-radius: 4px; padding: .35rem .6rem; font: inherit; width: 16rem; max-width: 100%; }
  .btn { appearance: none; cursor: pointer; color: var(--heading);
         background-image: linear-gradient(rgba(255,255,255,.12), rgba(255,255,255,0)), linear-gradient(#7a8288, #7a8288);
         border: 1px solid var(--comp-bd); border-radius: 4px; padding: .3rem .8rem; font: inherit; font-size: .8rem;
         text-shadow: 0 -1px 0 rgba(0,0,0,.3); }
  .btn:hover { background-image: linear-gradient(rgba(255,255,255,.18), rgba(255,255,255,.03)), linear-gradient(#7a8288, #7a8288); }
  .btn.recon { background-image: linear-gradient(rgba(255,255,255,.12), rgba(255,255,255,0)), linear-gradient(#d9534f, #c9302c); }
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
  .cards { display: flex; flex-wrap: wrap; gap: .8rem; margin-bottom: 1rem; }
  .card { background-image: linear-gradient(#3e444a, #3a3f44 60%, #363b40); border: 1px solid var(--comp-bd);
          border-radius: 4px; box-shadow: inset 0 1px 0 rgba(255,255,255,.06); padding: .65rem 1rem; min-width: 8.5rem; }
  .card .n { font-size: 1.7rem; font-weight: 500; color: var(--heading); font-variant-numeric: tabular-nums; }
  .card .l { color: var(--muted); font-size: .7rem; text-transform: uppercase; letter-spacing: .06em; }
  .card.live .n { color: var(--success); }
  .netbar { display: flex; flex-wrap: wrap; gap: .4rem; margin-bottom: 1rem; align-items: center; }
  .netbar .lbl { color: var(--muted); font-size: .72rem; text-transform: uppercase; letter-spacing: .06em; margin-right: .2rem; }
  .chip { display: inline-flex; align-items: center; gap: .35rem; cursor: pointer; max-width: 22rem;
          background: var(--panel2); border: 1px solid var(--comp-bd); border-radius: 12px; padding: .15rem .65rem;
          font-size: .8rem; color: var(--text); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .chip:hover { border-color: #5a6067; }
  .chip.active { background: #2c4a56; border-color: var(--info); color: var(--heading); }
  .chip .n { color: var(--muted); font-variant-numeric: tabular-nums; }
  .sw { display: inline-block; width: .65rem; height: .65rem; border-radius: 2px; flex: none; }
  .layout { display: grid; grid-template-columns: minmax(0, 1fr) 360px; gap: 1rem; align-items: start; }
  @media (max-width: 1050px) { .layout { grid-template-columns: minmax(0, 1fr); } }
  .panel { background: var(--panel); border: 1px solid var(--comp-bd); border-radius: 4px; overflow: hidden;
           box-shadow: inset 0 1px 0 rgba(255,255,255,.05); }
  .scroll { max-height: 68vh; overflow: auto; }
  table { border-collapse: collapse; width: 100%; font-size: .86rem; }
  th, td { text-align: left; padding: .42rem .7rem; vertical-align: middle; }
  thead th { position: sticky; top: 0; z-index: 1; background-image: linear-gradient(#41474d, #3a3f44);
             color: var(--heading); font-weight: 500; font-size: .7rem; text-transform: uppercase;
             letter-spacing: .05em; border-bottom: 2px solid var(--table-bd); white-space: nowrap; }
  th.sortable { cursor: pointer; }
  th.sortable:hover { color: var(--info); }
  tbody tr { border-top: 1px solid var(--table-bd); }
  tbody tr:nth-child(even) { background: rgba(255,255,255,.035); }
  tbody tr:hover { background: rgba(255,255,255,.075); }
  tr.click { cursor: pointer; }
  tr.sel, tr.sel:nth-child(even) { background: rgba(91,192,222,.14); }
  td.num { text-align: right; font-variant-numeric: tabular-nums; }
  td.mono, .mono { font-family: Menlo, Monaco, Consolas, 'Courier New', monospace; font-size: .82rem; }
  td.wrap { white-space: pre-wrap; word-break: break-word; }
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
  .b-none { background: rgba(255,255,255,.07); color: var(--muted); }
  .livecell { color: var(--success); white-space: nowrap; font-variant-numeric: tabular-nums; }
  .dot { display: inline-block; width: .55rem; height: .55rem; border-radius: 50%; margin-right: .35rem;
         background: var(--success); box-shadow: 0 0 6px rgba(98,196,98,.7); animation: pulse 1.2s ease-in-out infinite; }
  @keyframes pulse { 50% { opacity: .35; } }
  @media (prefers-reduced-motion: reduce) { .dot { animation: none; } }
  .side { position: sticky; top: .8rem; max-height: calc(100vh - 1.6rem); overflow: auto; padding: .9rem 1rem; }
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
  .ids { font-family: Menlo, Monaco, Consolas, monospace; font-size: .8rem; color: var(--text); }
  .grid2 { display: grid; grid-template-columns: repeat(auto-fill, minmax(330px, 1fr)); gap: .8rem; padding: .8rem; }
  .comm { background: var(--panel2); border: 1px solid var(--comp-bd); border-radius: 4px; padding: .6rem .75rem; }
  .comm h5 { margin: 0 0 .35rem; color: var(--heading); font-size: .85rem; font-weight: 500; }
  .comm .meta { color: var(--muted); font-size: .75rem; margin-bottom: .4rem; }
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
</style>
</head>
<body>
<header><div class="wrap hdr">
  <div>
    <h1><span class="accent">dsd-server</span> network explorer</h1>
    <div class="sub"><span id="live">connecting&hellip;</span> &middot; calls, talkgroups &amp; radios, associated per protocol
      &middot; <a href="/">status page</a></div>
  </div>
  <div class="hdr-actions">
    <input id="q" type="search" placeholder="Find radio, talkgroup, alias, text&hellip;" autocomplete="off">
    <span id="recst" class="recst"></span>
    <button id="rec" class="btn" type="button" title="Record everything the explorer receives, to replay and analyse offline">&#9679; Record</button>
    <button id="pause" class="btn" type="button">Pause</button>
    <button id="clear" class="btn" type="button" title="Forget everything learned so far">Clear</button>
  </div>
</div></header>
<div class="wrap">
  <div class="tabs famtabs" id="famtabs"></div>
  <div id="empty" class="emptybig"><b>Waiting for digital-voice traffic</b>
    Start a decode session (DMR, P25, NXDN, dPMR, D-STAR, YSF, TETRA, EDACS) and its calls, talkgroups,
    radios and networks will appear here as they are heard. Associations are kept per protocol.</div>
  <div id="main" hidden>
    <div class="cards" id="cards"></div>
    <div class="netbar" id="netbar"></div>
    <div class="layout">
      <div>
        <div class="tabs" id="viewtabs">
          <button class="tab" data-v="calls">Calls <span class="count" id="c-calls"></span></button>
          <button class="tab" data-v="tgs">Talkgroups <span class="count" id="c-tgs"></span></button>
          <button class="tab" data-v="radios">Radios <span class="count" id="c-radios"></span></button>
          <button class="tab" data-v="graph">Graph</button>
          <button class="tab" data-v="links">Links</button>
          <button class="tab" data-v="nets">Networks <span class="count" id="c-nets"></span></button>
        </div>
        <div class="panel" id="v-calls"><div class="scroll" id="t-calls"></div></div>
        <div class="panel" id="v-tgs" hidden><div class="scroll" id="t-tgs"></div></div>
        <div class="panel" id="v-radios" hidden><div class="scroll" id="t-radios"></div></div>
        <div class="panel" id="v-graph" hidden>
          <div class="gbar">
            <label>Nodes <select id="g-cap"><option>100</option><option selected>250</option><option>500</option><option>1000</option></select></label>
            <label><input type="checkbox" id="g-priv" checked> Private-call links</label>
            <label><input type="checkbox" id="g-labels" checked> Radio labels</label>
            <button class="btn" id="g-fit" type="button">Fit</button>
            <span id="g-note" style="color:var(--muted)"></span>
          </div>
          <div id="gwrap"><svg id="gsvg"></svg></div>
          <div class="legend" id="g-legend"></div>
        </div>
        <div class="panel" id="v-links" hidden><div class="scroll" id="t-links" style="max-height:78vh"></div></div>
        <div class="panel" id="v-nets" hidden><div class="scroll" id="t-nets"></div></div>
      </div>
      <aside class="panel side" id="detail"></aside>
    </div>
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
var S = { d: null, fam: null, net: '*', view: 'calls', sel: null, q: '', paused: false, skew: 0,
          ncol: {}, nidx: {}, sort: {} };
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
function now() { return Date.now() + S.skew; }
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
function live(c) { return c.open && now() - c.last < LIVE_MS; }

// ---------- indexing ----------
function colorFor(key) {
  var n = IX && IX.netByKey[key];
  if (!n || n.confidence === 'none') return '#7a8288';
  var ck = S.fam + '|' + key;
  if (!S.ncol[ck]) { var i = S.nidx[S.fam] || 0; S.ncol[ck] = PAL[i % PAL.length]; S.nidx[S.fam] = i + 1; }
  return S.ncol[ck];
}
function index() {
  var F = S.d.families[S.fam];
  var X = { F: F, nets: F.networks.slice(), netByKey: {}, tgs: F.talkgroups, tgById: {}, radios: F.radios,
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
function primaryNet(list) {
  var best = null;
  (list || []).forEach(function (k) { if (best == null || (IX.netRank[k] || 0) < (IX.netRank[best] || 0)) best = k; });
  return best;
}
function inNet(list) { return S.net === '*' || (list || []).indexOf(S.net) >= 0; }
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
function fCalls() {
  return IX.calls.filter(function (c) {
    return (S.net === '*' || c.net === S.net) && (!S.q || qm(c.src, c.tgt, c.alias, c.text, IX.rById[c.src] && IX.rById[c.src].aliases));
  });
}
function fTgs() { return IX.tgs.filter(function (t) { return inNet(t.networks) && (!S.q || qm(t.id)); }); }
function fRadios() { return IX.radios.filter(function (r) { return inNet(r.networks) && (!S.q || qm(r.id, r.aliases)); }); }

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
function netc(key) {
  var n = IX.netByKey[key];
  return h('span', { class: 'netc', title: n ? n.label : key,
                     onclick: function (e) { e.stopPropagation(); select('net', key); } }, [sw(key), n ? n.label : key]);
}
function nets(list) {
  var w = h('span');
  (list || []).forEach(function (k, i) { if (i) w.appendChild(document.createTextNode(' ')); w.appendChild(netc(k)); });
  return w;
}
function badge(cls, t) { return h('span', { class: 'badge ' + cls, text: t }); }
function confBadge(c) { return badge('b-' + c, c === 'none' ? 'unidentified' : c); }

// ---------- generic sortable table ----------
function table(cont, view, cols, rows, empty, onRow, selFn) {
  var st = S.sort[view];
  if (st && cols[st.i] && cols[st.i].k) {
    var kf = cols[st.i].k;
    rows = rows.slice().sort(function (a, b) {
      var x = kf(a), y = kf(b);
      if (typeof x === 'string' && typeof y === 'string') return x.localeCompare(y, undefined, { numeric: true }) * st.dir;
      return (x < y ? -1 : x > y ? 1 : 0) * st.dir;
    });
  }
  var head = h('tr', null, cols.map(function (c, i) {
    var arrow = st && st.i === i ? (st.dir > 0 ? ' ▲' : ' ▼') : '';
    return h('th', { class: (/\bnum\b/.test(c.cls || '') ? 'num' : '') + (c.k ? ' sortable' : ''),
                     onclick: c.k ? function () {
                       var cur = S.sort[view];
                       S.sort[view] = cur && cur.i === i ? { i: i, dir: -cur.dir } : { i: i, dir: c.dir || -1 };
                       renderView();
                     } : null }, c.label + arrow);
  }));
  var tb = h('tbody');
  if (!rows.length) tb.appendChild(h('tr', null, h('td', { class: 'empty', colspan: cols.length, text: empty })));
  rows.slice(0, ROWS).forEach(function (r) {
    tb.appendChild(h('tr', { class: (onRow ? 'click' : '') + (selFn && selFn(r) ? ' sel' : ''),
                             onclick: onRow ? function () { onRow(r); } : null },
      cols.map(function (c) { return h('td', { class: c.cls || '' }, c.cell(r)); })));
  });
  var y = cont.scrollTop;
  cont.textContent = '';
  cont.appendChild(h('table', null, [h('thead', null, head), tb]));
  if (rows.length > ROWS) cont.appendChild(h('div', { class: 'more', text: 'Showing ' + ROWS + ' of ' + rows.length +
                                                      ' — narrow it with the search box or a network filter.' }));
  cont.scrollTop = y;
}
function isSel(type, id) { return S.sel && S.sel.type === type && S.sel.id === id; }

// ---------- views ----------
function viewCalls() {
  var rows = fCalls();
  table($('t-calls'), 'calls', [
    { label: 'Start (UTC)', cls: 'mono nowrap', k: function (c) { return c.start; }, cell: function (c) { return hms(c.start); } },
    { label: 'Duration', cls: 'nowrap', k: function (c) { return c.last - c.start; },
      cell: function (c) {
        return live(c) ? h('span', { class: 'livecell' }, [h('span', { class: 'dot' }), dur(now() - c.start)])
                       : dur(c.last - c.start);
      } },
    { label: 'Network', cell: function (c) { return c.net ? netc(c.net) : '-'; } },
    { label: 'Slot', cls: 'num', k: function (c) { return c.slot; }, cell: function (c) { return c.slot || '—'; } },
    { label: 'From', k: function (c) { return c.src; }, cell: function (c) { return c.src ? rlink(c.src, c.alias || null) : '—'; } },
    { label: 'To', k: function (c) { return c.tgt; },
      cell: function (c) { return !c.tgt ? '—' : c.priv ? h('span', null, ['⇄ ', rlink(c.tgt)]) : tlink(c.tgt); } },
    { label: 'Type', cls: 'nowrap', cell: function (c) {
        return h('span', null, [
          (c.data && !c.voice) ? badge('b-data', 'DATA') : badge('b-voice', 'VOICE'),
          c.priv ? badge('b-priv', 'PRIVATE') : badge('b-group', 'GROUP'),
          c.emerg ? badge('b-emerg', 'EMERGENCY') : null, c.enc ? badge('b-enc', 'ENCRYPTED') : null,
          c.streams > 1 ? h('span', { class: 'badge b-group', title: 'Heard by ' + c.streams + ' receivers (one call, deduplicated)' },
                            c.streams + ' RX') : null]);
      } },
    { label: 'Text', cls: 'wrap', cell: function (c) { return c.text || ''; } }
  ], rows, 'No calls heard yet' + (S.net !== '*' || S.q ? ' for this filter.' : '.'), null, null);
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
    { label: 'Network', k: function (n) { return n.label; }, dir: 1, cell: function (n) { return netc(n.key); } },
    { label: 'Identity', cell: function (n) { return confBadge(n.confidence); } },
    { label: 'Identifiers', cls: 'ids', cell: function (n) { return keys(n.ids).map(function (k) { return k + '=' + n.ids[k]; }).join('  '); } },
    { label: 'Sites', cell: function (n) { return n.sites.join(', ') || '—'; } },
    { label: 'TGs', cls: 'num', k: function (n) { return IX.netTg[n.key] || 0; }, cell: function (n) { return IX.netTg[n.key] || 0; } },
    { label: 'Radios', cls: 'num', k: function (n) { return IX.netRad[n.key] || 0; }, cell: function (n) { return IX.netRad[n.key] || 0; } },
    { label: 'Calls', cls: 'num', k: function (n) { return n.calls; }, cell: function (n) { return n.calls; } },
    { label: 'Streams', cls: 'num', k: function (n) { return n.sessions; }, cell: function (n) { return n.sessions; } },
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
function recent(filter) {
  var cs = IX.calls.filter(filter).slice(0, 12);
  if (!cs.length) return h('div', { class: 'hint', text: 'No calls in the recent buffer.' });
  var ul = h('ul', { class: 'lst' });
  cs.forEach(function (c) {
    ul.appendChild(h('li', null, [
      h('div', null, [c.src ? rlink(c.src, c.alias || null) : '?', ' → ', c.tgt ? (c.priv ? rlink(c.tgt) : tlink(c.tgt)) : '?',
                      c.emerg ? h('span', null, [' ', badge('b-emerg', 'EMERG')]) : null,
                      c.text ? h('div', { class: 'alias', text: '“' + c.text + '”' }) : null]),
      h('span', { class: 'c', text: live(c) ? 'live' : hms(c.start) })]));
  });
  return ul;
}
function renderDetail() {
  var d = $('detail');
  d.textContent = '';
  var sel = S.sel;
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
    d.appendChild(kv([['Calls', t.calls], ['Radios', keys(t.radios).length], ['Emergency', t.emerg],
                      ['Encrypted', t.enc], ['First', ago(t.first)], ['Last', ago(t.last)]]));
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
    d.appendChild(kv([['Calls', r.calls], ['Talkgroups', keys(r.tgs).length], ['Private peers', keys(r.peers).length],
                      ['Networks', r.networks.length], ['First', ago(r.first)], ['Last', ago(r.last)]]));
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
      h('button', { class: 'btn', type: 'button', onclick: function () { setNet(S.net === n.key ? '*' : n.key); } },
        S.net === n.key ? 'Show all networks' : 'Show only this network')]));
    d.appendChild(kv([['Calls', n.calls], ['Talkgroups', IX.netTg[n.key] || 0], ['Radios', IX.netRad[n.key] || 0],
                      ['Streams', n.sessions], ['First', ago(n.first)], ['Last', ago(n.last)]]));
    d.appendChild(h('h4', { text: 'Identifiers' }));
    d.appendChild(h('div', { class: 'ids', text: keys(n.ids).map(function (k) { return k + '=' + n.ids[k]; }).join('   ') || '—' }));
    d.appendChild(h('h4', { text: 'Sites' }));
    d.appendChild(h('div', { text: n.sites.join(', ') || 'None reported.' }));
    if (n.confidence !== 'strong')
      d.appendChild(h('div', { class: 'hint', style: 'margin-top:.6rem;font-size:.8rem',
        text: n.confidence === 'weak'
          ? 'Weak identity: only a short code (color code / NAC / RAN) was seen, which unrelated systems can share, so this bucket covers just one stream. Talkgroups and radios it shares with other buckets are listed under Links.'
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
var GR = { nodes: [], links: [], by: {}, adj: {}, sig: '', t: { k: 1, x: 0, y: 0 }, alpha: 0, raf: 0, fitted: false,
           root: null, lg: null, ng: null, drag: null, pan: null };
function gSig() {
  return [S.d.version, S.fam, S.net, S.q, $('g-cap').value, $('g-priv').checked].join('|');
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
  GR.nodes = nodes; GR.links = links; GR.by = by;
  $('g-note').textContent = nodes.length + ' nodes · ' + links.length + ' links' +
    (cand.length > cap ? ' (busiest ' + cap + ' of ' + cand.length + ')' : '');
  drawGraph();
  // Every voice frame bumps the model version; only re-heat the layout when
  // the node/link set itself changed, so a busy call doesn't keep it jiggling.
  var structure = nodes.map(function (x) { return x.id; }).sort().join(',') + '|' +
                  links.map(function (l) { return l.a + '>' + l.b; }).sort().join(',');
  if (famChanged) GR.fitted = false;
  if (structure !== GR.structure || famChanged) {
    GR.structure = structure;
    GR.alpha = Math.max(GR.alpha, famChanged || !GR.fitted ? 1 : 0.35);
  }
  run();
}
function nodeColor(n) { var k = primaryNet(n.ref.networks); return k ? colorFor(k) : '#7a8288'; }
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
      e.stopPropagation();
      GR.drag = { n: n, moved: false, sx: e.clientX, sy: e.clientY };
      svg.setPointerCapture(e.pointerId);
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
      var B = N[j], dx = B.x - A.x, dy = B.y - A.y, d2 = dx * dx + dy * dy;
      if (d2 > 160000) continue;
      if (d2 < 1) { dx = Math.random() - .5; dy = Math.random() - .5; d2 = 1; }
      var d = Math.sqrt(d2), f = 2600 * a / d2, ux = dx / d * f, uy = dy / d * f;
      A.vx -= ux; A.vy -= uy; B.vx += ux; B.vy += uy;
    }
  }
  L.forEach(function (l) {
    var A = GR.by[l.a], B = GR.by[l.b], dx = B.x - A.x, dy = B.y - A.y, d = Math.sqrt(dx * dx + dy * dy) || 1;
    var rest = l.priv ? 55 : 75, f = (d - rest) * 0.05 * a * Math.min(2, 0.6 + Math.log2(1 + l.w) * 0.3);
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
      GR.raf = 0;
      if (S.view === 'graph' && !GR.fitted && GR.nodes.length) { fit(); GR.fitted = true; }
      return;
    }
    for (var k = 0; k < 2; k++) step();
    if (!GR.fitted && GR.alpha < 0.2 && GR.nodes.length) { fit(); GR.fitted = true; }
    paint();
    GR.raf = requestAnimationFrame(frame);
  })();
}
function fit() {
  var svg = $('gsvg'), w = svg.clientWidth || 800, hh = svg.clientHeight || 600;
  if (!GR.nodes.length) return;
  var x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  GR.nodes.forEach(function (n) { x0 = Math.min(x0, n.x); y0 = Math.min(y0, n.y); x1 = Math.max(x1, n.x); y1 = Math.max(y1, n.y); });
  // Cap the zoom so a small graph isn't blown up until its labels collide.
  var k = Math.min(1.35, Math.max(0.15, Math.min(w / (x1 - x0 + 160), hh / (y1 - y0 + 100))));
  GR.t = { k: k, x: -(x0 + x1) / 2 * k, y: -(y0 + y1) / 2 * k };
  paint();
}
function toGraph(cx, cy) {
  var svg = $('gsvg'), r = svg.getBoundingClientRect();
  return { x: (cx - r.left - r.width / 2 - GR.t.x) / GR.t.k, y: (cy - r.top - r.height / 2 - GR.t.y) / GR.t.k };
}
(function wireGraph() {
  var svg = $('gsvg');
  svg.addEventListener('pointerdown', function (e) {
    if (GR.drag) return;
    GR.pan = { x: e.clientX, y: e.clientY, tx: GR.t.x, ty: GR.t.y, moved: false };
    svg.classList.add('panning');
    svg.setPointerCapture(e.pointerId);
  });
  svg.addEventListener('pointermove', function (e) {
    if (GR.drag) {
      var dg = GR.drag;
      if (Math.abs(e.clientX - dg.sx) + Math.abs(e.clientY - dg.sy) > 3) dg.moved = true;
      if (!dg.moved) return;
      var p = toGraph(e.clientX, e.clientY);
      dg.n.x = p.x; dg.n.y = p.y;
      GR.alpha = Math.max(GR.alpha, 0.25); run(); paint();
    } else if (GR.pan) {
      var dx = e.clientX - GR.pan.x, dy = e.clientY - GR.pan.y;
      if (Math.abs(dx) + Math.abs(dy) > 3) GR.pan.moved = true;
      GR.t.x = GR.pan.tx + dx; GR.t.y = GR.pan.ty + dy; paint();
    }
  });
  function up() {
    if (GR.drag) {
      var dg = GR.drag; GR.drag = null;
      if (!dg.moved) select(dg.n.kind === 'tg' ? 'tg' : 'radio', dg.n.ref.id);
    } else if (GR.pan) {
      if (!GR.pan.moved && S.sel) { S.sel = null; renderDetail(); highlight(); }
      GR.pan = null;
    }
    svg.classList.remove('panning');
  }
  svg.addEventListener('pointerup', up);
  svg.addEventListener('pointercancel', up);
  svg.addEventListener('wheel', function (e) {
    e.preventDefault();
    var r = svg.getBoundingClientRect(), cx = e.clientX - r.left - r.width / 2, cy = e.clientY - r.top - r.height / 2;
    var k0 = GR.t.k, k1 = Math.max(0.1, Math.min(6, k0 * Math.exp(-e.deltaY * 0.0015)));
    GR.t.x = cx - (cx - GR.t.x) * k1 / k0; GR.t.y = cy - (cy - GR.t.y) * k1 / k0; GR.t.k = k1;
    paint();
  }, { passive: false });
  ['g-cap', 'g-priv'].forEach(function (id) { $(id).addEventListener('change', function () { GR.sig = ''; buildGraph(); }); });
  $('g-labels').addEventListener('change', function () { drawGraph(); });
  $('g-fit').addEventListener('click', fit);
  window.addEventListener('resize', function () { if (S.view === 'graph') paint(); });
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
  S.sel = isSel(type, id) ? null : { type: type, id: id };
  renderDetail();
  if (S.view === 'graph') highlight(); else renderView();
}
function setNet(k) { S.net = k; GR.sig = ''; render(); }
function setView(v) {
  S.view = v; store('view', v);
  var tabs = document.querySelectorAll('#viewtabs .tab');
  for (var i = 0; i < tabs.length; i++) tabs[i].classList.toggle('active', tabs[i].getAttribute('data-v') === v);
  ['calls', 'tgs', 'radios', 'graph', 'links', 'nets'].forEach(function (x) { $('v-' + x).hidden = x !== v; });
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
}
function famTotals(f) { var F = S.d.families[f]; return F.calls.length + F.talkgroups.length + F.radios.length; }
function render() {
  var fams = Object.keys(S.d.families).sort(function (a, b) { return famTotals(b) - famTotals(a); });
  $('empty').hidden = fams.length > 0;
  $('main').hidden = !fams.length;
  var ft = $('famtabs');
  ft.textContent = '';
  if (!fams.length) { IX = null; return; }
  if (fams.indexOf(S.fam) < 0) { S.fam = fams[0]; S.net = '*'; S.sel = null; }
  fams.forEach(function (f) {
    var F = S.d.families[f], lv = F.calls.filter(live).length;
    ft.appendChild(h('button', { class: 'tab' + (f === S.fam ? ' active' : ''), type: 'button',
      onclick: function () { S.fam = f; store('fam', f); S.net = '*'; S.sel = null; GR.sig = ''; render(); } },
      [FAMN[f] || f.toUpperCase(), ' ', h('span', { class: 'count', text: F.radios.length + ' radios' + (lv ? ' · ' + lv + ' live' : '') })]));
  });
  index();
  if (S.net !== '*' && !IX.netByKey[S.net]) S.net = '*';
  var calls = fCalls(), tgs = fTgs(), radios = fRadios(), lv = calls.filter(live).length, sites = {};
  IX.nets.forEach(function (n) { if (S.net === '*' || n.key === S.net) n.sites.forEach(function (s) { sites[n.key + s] = 1; }); });
  var cards = $('cards');
  cards.textContent = '';
  [['Networks', S.net === '*' ? IX.nets.length : 1], ['Sites', Object.keys(sites).length], ['Talkgroups', tgs.length],
   ['Radios', radios.length], ['Calls (recent)', calls.length], ['Live calls', lv]].forEach(function (c, i) {
    cards.appendChild(h('div', { class: 'card' + (i === 5 && lv ? ' live' : '') }, [h('div', { class: 'n', text: String(c[1]) }), h('div', { class: 'l', text: c[0] })]));
  });
  var nb = $('netbar');
  nb.textContent = '';
  nb.appendChild(h('span', { class: 'lbl', text: 'Network' }));
  nb.appendChild(h('span', { class: 'chip' + (S.net === '*' ? ' active' : ''), onclick: function () { setNet('*'); } }, 'All networks'));
  IX.nets.slice().sort(function (a, b) { return b.calls - a.calls; }).forEach(function (n) {
    nb.appendChild(h('span', { class: 'chip' + (S.net === n.key ? ' active' : ''), title: n.label + ' — ' + n.confidence + ' identity',
      onclick: function () { setNet(S.net === n.key ? '*' : n.key); select('net', n.key); } },
      [sw(n.key), n.label, h('span', { class: 'n', text: String(n.calls) })]));
  });
  $('c-calls').textContent = calls.length; $('c-tgs').textContent = tgs.length;
  $('c-radios').textContent = radios.length; $('c-nets').textContent = IX.nets.length;
  renderView();
  renderDetail();
}

// ---------- recording (Record button) ----------
function mb(n) { return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB'; }
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

function poll() {
  if (S.paused || document.hidden) { setTimeout(poll, POLL); return; }
  fetch('/net.json', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (d) {
    S.skew = d.now - Date.now();
    updateRec(d.rec);
    var changed = !S.d || d.version !== S.d.version;
    S.d = d;
    $('live').textContent = 'live · updated ' + hms(d.now) + 'Z';
    if (changed || S.view === 'calls') render();
    else { renderDetail(); }
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
  if (!confirm('Forget all calls, talkgroups, radios and networks learned so far?')) return;
  fetch('/net/clear', { cache: 'no-store' }).then(function () { S.sel = null; S.net = '*'; GR.sig = ''; });
});
S.fam = load('fam');
setView(load('view') || 'calls');
poll();
})();
</script>
</body>
</html>
)NETPAGE";
}

} // namespace dsdsrv
