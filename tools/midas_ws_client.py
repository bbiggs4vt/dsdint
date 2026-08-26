#!/usr/bin/env python3
"""Test client for dsd-server: streams IQ from a MIDAS BLUE file.

Reads an X-Midas BLUE file (attached header), extracts complex IQ
samples, and streams them to a running dsd-server over its WebSocket
protocol (see PROTOCOL.md at the repo root). Text frames from the
server -- started/event/error JSON -- are printed to stdout as they
arrive; tagged binary audio frames (0x01 + int16 LE PCM) are written to
a WAV file.

Stdlib only -- the WebSocket client is implemented by hand (RFC 6455
subset: client handshake, masked frames, ping/pong, close), so no pip
installs are needed. numpy is used for fast sample conversion when
available, with a pure-Python fallback.

Usage:
    python3 tools/midas_ws_client.py capture.tmp
    python3 tools/midas_ws_client.py capture.tmp --host 127.0.0.1 --port 8765 \
        --wav out.wav --afc --speed 4
    python3 tools/midas_ws_client.py capture.tmp --info   # dump header, no network

BLUE file support:
  - Attached headers only (detached .det/.prm pairs are rejected with a
    clear error).
  - Complex data formats: CB (int8), CI (int16), CL (int32), CF
    (float32), CD (float64). Scalar formats are rejected -- the server
    needs IQ. Samples are normalized to +/-1.0 float32 for the wire.
  - Sample rate comes from the type-1000/2000 adjunct's xdelta
    (rate = 1/xdelta); --sample-rate overrides it.

WAV output:
  - Audio from the server is 8000 Hz int16 PCM (see PROTOCOL.md).
    Channel count is inferred from the backend: the dsd-fme subprocess
    backend (started.udp_audio_port != 0) sends stereo interleaved
    (slot 1 left / slot 2 right); the DSDcc backend (udp_audio_port ==
    0) sends mono. --channels overrides the inference.
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import wave

try:
    import numpy as _np
except ImportError:  # pure-Python fallback below
    _np = None


# --------------------------------------------------------------- BLUE

# (mode char, type char) -> (struct code, bytes per scalar, full-scale divisor)
_FORMAT_SCALARS = {
    "B": ("b", 1, 128.0),
    "I": ("h", 2, 32768.0),
    "L": ("i", 4, 2147483648.0),
    "F": ("f", 4, None),  # already floating point
    "D": ("d", 8, None),
}


class BlueFile:
    """Minimal attached-header X-Midas BLUE file reader."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            hdr = f.read(512)
        if len(hdr) < 512 or hdr[0:4] != b"BLUE":
            raise ValueError(f"{path}: not a BLUE file (bad magic {hdr[0:4]!r})")

        head_rep = hdr[4:8]
        if head_rep == b"EEEI":
            he = "<"
        elif head_rep == b"IEEE":
            he = ">"
        else:
            raise ValueError(f"{path}: unknown head_rep {head_rep!r}")

        data_rep = hdr[8:12]
        if data_rep == b"EEEI":
            self.data_endian = "<"
        elif data_rep == b"IEEE":
            self.data_endian = ">"
        else:
            raise ValueError(f"{path}: unknown data_rep {data_rep!r}")

        (detached,) = struct.unpack_from(he + "i", hdr, 12)
        if detached:
            raise ValueError(
                f"{path}: detached-header BLUE files are not supported -- "
                "point this tool at a file with an attached data block")

        (self.data_start,) = struct.unpack_from(he + "d", hdr, 32)
        (self.data_size,) = struct.unpack_from(he + "d", hdr, 40)
        (self.type,) = struct.unpack_from(he + "i", hdr, 48)
        self.format = hdr[52:54].decode("ascii", "replace")

        # Type 1000 and 2000 adjuncts both put xstart/xdelta first.
        (self.xstart,) = struct.unpack_from(he + "d", hdr, 256)
        (self.xdelta,) = struct.unpack_from(he + "d", hdr, 264)

        mode, kind = self.format[0].upper(), self.format[1].upper()
        if mode != "C":
            raise ValueError(
                f"{path}: format {self.format!r} is not complex -- the server "
                "needs IQ (complex) data")
        if kind not in _FORMAT_SCALARS:
            raise ValueError(f"{path}: unsupported scalar type {kind!r} in format {self.format!r}")
        self.scalar_code, self.scalar_bytes, self.full_scale = _FORMAT_SCALARS[kind]
        self.frame_bytes = 2 * self.scalar_bytes  # one complex sample

        file_size = os.path.getsize(path)
        if self.data_size <= 0 or self.data_start + self.data_size > file_size:
            # Fall back to "rest of the file" for slightly off headers.
            self.data_size = file_size - self.data_start
        self.n_samples = int(self.data_size) // self.frame_bytes

    @property
    def sample_rate(self):
        return 1.0 / self.xdelta if self.xdelta > 0 else None

    def describe(self):
        rate = self.sample_rate
        return (
            f"{self.path}: BLUE type {self.type}, format {self.format}, "
            f"{self.n_samples} complex samples, "
            f"sample_rate={rate:.1f} Hz" if rate else "sample_rate=unknown (xdelta<=0)")

    def iter_iq_float32_le(self, block_samples):
        """Yield bytes of interleaved little-endian float32 IQ, normalized to +/-1."""
        endian_np = {"<": "<", ">": ">"}[self.data_endian]
        with open(self.path, "rb") as f:
            f.seek(int(self.data_start))
            remaining = self.n_samples
            while remaining > 0:
                n = min(block_samples, remaining)
                raw = f.read(n * self.frame_bytes)
                if len(raw) < self.frame_bytes:
                    return
                n = len(raw) // self.frame_bytes
                remaining -= n
                nscalars = n * 2
                if _np is not None:
                    dt = _np.dtype(endian_np + self.scalar_code)
                    arr = _np.frombuffer(raw, dtype=dt, count=nscalars).astype(_np.float32)
                    if self.full_scale:
                        arr = arr / _np.float32(self.full_scale)
                    yield arr.astype("<f4").tobytes()
                else:
                    vals = struct.unpack(
                        f"{self.data_endian}{nscalars}{self.scalar_code}", raw[: nscalars * self.scalar_bytes])
                    if self.full_scale:
                        fs = self.full_scale
                        vals = [v / fs for v in vals]
                    yield struct.pack(f"<{nscalars}f", *vals)


# ---------------------------------------------------------- WebSocket

class WSClient:
    """Minimal RFC 6455 client: handshake, masked send, frame reader."""

    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed during WebSocket handshake")
            resp += chunk
        status = resp.split(b"\r\n", 1)[0].decode("latin1")
        if "101" not in status:
            raise ConnectionError(f"WebSocket handshake rejected: {status}")
        self._send_lock = threading.Lock()

    def _send_frame(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | opcode])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self._send_lock:
            self.sock.sendall(hdr + mask + masked)

    def send_text(self, s):
        self._send_frame(0x1, s.encode())

    def send_binary(self, b):
        self._send_frame(0x2, b)

    def send_close(self):
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass

    def _recv_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("connection closed")
            data += chunk
        return data

    def read_frame(self):
        """Returns (opcode, payload) for the next data frame; handles ping/pong.
        Returns (None, None) on clean close."""
        while True:
            h = self._recv_exact(2)
            opcode = h[0] & 0x0F
            n = h[1] & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._recv_exact(8))[0]
            payload = self._recv_exact(n) if n else b""
            if opcode == 0x9:  # ping -> pong
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:  # unsolicited pong
                continue
            if opcode == 0x8:  # close
                self.send_close()
                return None, None
            return opcode, payload

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# -------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description="Stream IQ from a MIDAS BLUE file to dsd-server; print "
                    "events, save decoded audio to WAV.")
    ap.add_argument("bluefile", help="BLUE file with complex IQ data (attached header)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--wav", default="dsd_audio.wav", help="output WAV path (default dsd_audio.wav)")
    ap.add_argument("--sample-rate", type=float, default=None,
                    help="override the sample rate from the BLUE header")
    ap.add_argument("--channel-bandwidth", type=float, default=12500.0)
    ap.add_argument("--freq-offset", type=float, default=0.0,
                    help="channel offset in Hz (positive = channel above 0 Hz)")
    ap.add_argument("--gain", type=float, default=26000.0)
    ap.add_argument("--afc", action="store_true", help="enable server-side AFC")
    ap.add_argument("--protocol", default=None,
                    help="advisory protocol hint for the server "
                         "(dmr, nxdn48, nxdn96, auto/unknown); omit to use the "
                         "server default")
    ap.add_argument("--speed", type=float, default=4.0,
                    help="streaming speed as a multiple of realtime; 0 = unpaced (default 4)")
    ap.add_argument("--block", type=int, default=4096, help="complex samples per frame (default 4096)")
    ap.add_argument("--channels", type=int, choices=(1, 2), default=None,
                    help="WAV channels; default: inferred from backend (dsd-fme=2, DSDcc=1)")
    ap.add_argument("--linger", type=float, default=3.0,
                    help="seconds to keep listening after the last IQ frame (default 3)")
    ap.add_argument("--info", action="store_true", help="print the BLUE header summary and exit")
    args = ap.parse_args()

    blue = BlueFile(args.bluefile)
    print(blue.describe())
    if args.info:
        return 0

    sample_rate = args.sample_rate or blue.sample_rate
    if not sample_rate:
        print("error: BLUE header has no usable xdelta; pass --sample-rate", file=sys.stderr)
        return 2

    ws = WSClient(args.host, args.port)
    print(f"connected to ws://{args.host}:{args.port}/")

    stats = {"text": 0, "audio_frames": 0, "audio_samples": 0}
    wav_channels = [args.channels]  # boxed; reader thread may infer it
    wav = wave.open(args.wav, "wb")
    wav_ready = threading.Event()
    done = threading.Event()

    def finish_wav_setup(channels):
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        wav_ready.set()

    def reader():
        try:
            while not done.is_set():
                opcode, payload = ws.read_frame()
                if opcode is None:
                    break
                if opcode == 0x1:  # text
                    text = payload.decode("utf-8", "replace")
                    print(text, flush=True)
                    stats["text"] += 1
                    # Infer WAV channel count from the backend on "started"
                    if wav_channels[0] is None:
                        try:
                            obj = json.loads(text)
                            if obj.get("type") == "started":
                                wav_channels[0] = 2 if obj.get("udp_audio_port", 0) else 1
                                finish_wav_setup(wav_channels[0])
                        except (ValueError, KeyError):
                            pass
                elif opcode == 0x2 and payload[:1] == b"\x01":
                    pcm = payload[1:]
                    if not wav_ready.is_set():
                        # started never told us (shouldn't happen); default stereo
                        wav_channels[0] = wav_channels[0] or 2
                        finish_wav_setup(wav_channels[0])
                    wav.writeframes(pcm)
                    stats["audio_frames"] += 1
                    stats["audio_samples"] += len(pcm) // 2
        except (ConnectionError, OSError) as e:
            if not done.is_set():
                print(f"[reader] connection ended: {e}", file=sys.stderr)

    if args.channels is not None:
        finish_wav_setup(args.channels)
    t = threading.Thread(target=reader, daemon=True)
    t.start()

    start_msg = {
        "type": "start",
        "sample_rate": sample_rate,
        "channel_bandwidth": args.channel_bandwidth,
        "freq_offset": args.freq_offset,
        "gain": args.gain,
        "afc": bool(args.afc),
    }
    if args.protocol is not None:
        start_msg["protocol"] = args.protocol
    ws.send_text(json.dumps(start_msg))

    block_seconds = args.block / sample_rate
    pace = block_seconds / args.speed if args.speed > 0 else 0.0
    sent = 0
    t0 = time.time()
    for chunk in blue.iter_iq_float32_le(args.block):
        ws.send_binary(chunk)
        sent += len(chunk) // 8
        if pace:
            time.sleep(pace)
    elapsed = time.time() - t0
    print(f"[client] sent {sent} IQ samples "
          f"({sent / sample_rate:.1f} s of signal in {elapsed:.1f} s)", flush=True)

    time.sleep(max(args.linger, 0.0))
    ws.send_text(json.dumps({"type": "stop"}))
    time.sleep(0.5)
    done.set()
    ws.send_close()
    ws.close()
    t.join(timeout=2)

    if not wav_ready.is_set():
        finish_wav_setup(wav_channels[0] or 1)
    wav.close()

    ch = wav_channels[0] or 1
    secs = stats["audio_samples"] / (8000.0 * ch)
    print(f"[client] {stats['text']} text frames; {stats['audio_frames']} audio frames "
          f"-> {args.wav} ({stats['audio_samples']} samples, {secs:.1f} s at 8 kHz "
          f"{'stereo' if ch == 2 else 'mono'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
