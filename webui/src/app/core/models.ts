// Shapes of the JSON documents dsd-server serves (see PROTOCOL.md). Field
// names follow the server's JSON exactly.

// ---- /status.json, /log.json ------------------------------------------------
export interface SessionRow {
  id: number;
  remote: string;
  protocol: string;
  chain: string;
  protocols_used: string[];
  protocols_requested: string[];
  active: boolean;
  connected: string;
  duration_seconds: number;
}
export interface HistoryRow {
  id: number;
  remote: string;
  protocol: string;
  chain: string;
  connected: string;
  ended: string;
  protocols_used: string[];
  protocols_requested: string[];
  duration_seconds: number;
}
export interface ProtocolUsage {
  protocol: string;
  chain: string;
  requests: number;
  starts: number;
  failed: number;
  sessions: number;
  active: number;
  decode_seconds: number;
  first_requested: string;
  last_requested: string;
}
export interface StatusDoc {
  total_sessions: number;
  current_sessions: number;
  active_pipelines: number;
  uptime_seconds: number;
  log_lines: number;
  iq_logging: boolean;
  started: string;
  by_protocol: Record<string, number>;
  sessions: SessionRow[];
  history: HistoryRow[];
  protocols: ProtocolUsage[];
}
export interface LogEntry { session: number; time: string; text: string; }
export interface LogDoc { log: LogEntry[]; }

// ---- /net.json and explorer exports ------------------------------------------
export type Confidence = 'strong' | 'channel' | 'weak' | 'none';
export interface NetNetwork {
  key: string;
  label: string;
  confidence: Confidence;
  ids: Record<string, string>;
  sites: string[];
  freqs?: number[];
  sessions: number;
  calls: number;
  first: number;
  last: number;
}
export interface NetTalkgroup {
  id: string;
  networks: string[];
  radios: Record<string, number>;
  calls: number;
  emerg: number;
  enc: number;
  first: number;
  last: number;
}
export interface NetRadio {
  id: string;
  aliases: string[];
  tgs: Record<string, number>;
  peers: Record<string, number>;
  networks: string[];
  calls: number;
  first: number;
  last: number;
}
export interface NetCall {
  id: number;
  session: number;
  net: string;
  site: string;
  freq: number;
  slot: string;
  src: string;
  tgt: string;
  alias: string;
  text: string;
  priv: boolean;
  voice: boolean;
  data: boolean;
  emerg: boolean;
  enc: boolean;
  open: boolean;
  streams: number;
  start: number;
  last: number;
  audio?: string;
  audio_ms?: number;
}
export interface NetFamily {
  networks: NetNetwork[];
  talkgroups: NetTalkgroup[];
  radios: NetRadio[];
  calls: NetCall[];
}
export interface RecStatus {
  on?: boolean;
  truncated?: boolean;
  file?: string;
  path?: string;
  bytes?: number;
  file_bytes?: number;
}
export interface AudioStatus {
  on?: boolean;
  dir?: string;
  bytes?: number;
  cap_bytes?: number;
  files?: number;
  recording?: number;
}
export interface Rate { per_s_1m: number; per_s_10m: number; total: number; }
export interface Source { instance?: string; name?: string; since?: number; through?: number; }
export interface ImportInfo {
  id: number;
  label: string;
  exported: number;
  sources: Source[];
  networks: number;
  talkgroups: number;
  radios: number;
  calls: number;
}
export interface NetDoc {
  version: number;
  now: number;
  instance?: string;
  name?: string;
  since?: number;
  rec?: RecStatus;
  audio?: AudioStatus;
  max_calls?: number;
  rates?: Record<string, Rate>;
  imports?: ImportInfo[];
  families: Record<string, NetFamily>;
}
// An explorer export (format "dsd-net-export"), as opened from a file.
export interface NetExport {
  format?: string;
  format_version?: number;
  exported?: number;
  now?: number;
  name?: string;
  source?: string;
  sources?: Source[];
  families: Record<string, NetFamily>;
}
export interface MergeReportItem { name: string; status: string; message: string; }
export interface MergeResponse { error?: string; report: MergeReportItem[]; export: NetExport; }
export interface ImportResult { name: string; status: string; message: string; id?: number; }

// ---- /net/asr/config.json ------------------------------------------------------
export interface AsrConfig {
  local: boolean;
  lib: boolean;
  models: string[];
  model: string;
  language: string;
  dir?: string;
}
