#!/usr/bin/env python3
"""Capacity measurement: N concurrent realtime streams against dsd-server.

Spawns the given server binary, drives it with N parallel realtime
(--speed 1) copies of a BLUE file via tools/midas_ws_client.py, and
reports the server's CPU cost per stream-second -- the number that
answers "how many streams fit on this box". CPU is measured over the
server's whole process tree including reaped dsd-fme children, so both
backends are measured fairly. Also reports peak RSS and whether every
stream's audio decoded fully.

Run it on the deployment hardware itself for real capacity numbers:

    python3 tools/stream_load_test.py build/dsd-server-dsdcc capture_32k.tmp 8
    python3 tools/stream_load_test.py build/dsd-server capture_32k.tmp 8 \
        --fme-path /path/to/dsd-fme-dir

To measure at a different IQ sample rate, pass the raw 48 kHz
discriminator capture plus --rate; the test IQ is generated on the fly:

    python3 tools/stream_load_test.py build/dsd-server-dsdcc dmr_it_8.dis 8 \
        --rate 64000

Streams/core budget arithmetic: a stream costs the reported CPU-ms per
stream-second; a core provides 1000 ms per second; keep ~25-30%
headroom. E.g. 8 cores at 35 ms/stream-s -> 8*1000*0.72/35 ~= 165
stream capacity on that machine.

Interpreting the WAV check: it passes when each stream's WAV holds
>60% of the file's duration in audio -- expected for a voice-active
capture, and expectedly 0/N for a noise/idle capture (measuring idle
cost is a legitimate use; ignore the check there).
"""

import argparse
import os
import subprocess
import sys
import time
import wave


def tree_cpu_and_rss(pid):
    """(cpu_seconds, rss_bytes) over pid + live descendants; CPU includes
    already-reaped children via cutime/cstime."""
    hz = os.sysconf("SC_CLK_TCK")
    total_cpu, total_rss, stack = 0.0, 0, [pid]
    while stack:
        p = stack.pop()
        try:
            with open(f"/proc/{p}/stat") as f:
                parts = f.read().rsplit(")", 1)[1].split()
            total_cpu += (int(parts[11]) + int(parts[12])
                          + int(parts[13]) + int(parts[14])) / hz
            with open(f"/proc/{p}/statm") as f:
                total_rss += int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
            with open(f"/proc/{p}/task/{p}/children") as f:
                stack += [int(c) for c in f.read().split()]
        except (FileNotFoundError, ProcessLookupError, IndexError, ValueError):
            pass
    return total_cpu, total_rss


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("server", help="path to dsd-server or dsd-server-dsdcc")
    ap.add_argument("bluefile",
                    help="BLUE IQ file to stream -- or, with --rate, a raw 48 kHz "
                         "S16LE discriminator capture (.dis) to FM-wrap first")
    ap.add_argument("n", type=int, help="number of concurrent streams")
    ap.add_argument("--rate", type=float, default=None,
                    help="treat the input as a 48 kHz discriminator capture and "
                         "generate the test IQ at this sample rate before the "
                         "run (uses make_test_bluefile.py; e.g. --rate 64000)")
    ap.add_argument("--port", type=int, default=18910)
    ap.add_argument("--fme-path", default=None,
                    help="directory containing dsd-fme, prepended to the server's PATH")
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 4,
                    help="server io threads (default: core count)")
    ap.add_argument("--keep-wavs", action="store_true")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    client = os.path.join(here, "midas_ws_client.py")

    # Input handling: a BLUE file streams as-is; with --rate, the input
    # is a raw discriminator capture that gets FM-wrapped into IQ at the
    # requested rate first. Mixing them up produces clear errors either
    # way (a BLUE file plus --rate is ambiguous; a .dis without --rate
    # fails BLUE parsing with a pointer here).
    with open(args.bluefile, "rb") as f:
        is_blue = f.read(4) == b"BLUE"
    if args.rate is not None:
        if is_blue:
            print("error: --rate regenerates IQ from a raw .dis capture, but the "
                  "input is already a BLUE file -- pass the .dis instead "
                  "(the loadtest Docker image ships one at "
                  "/opt/dsd-server/testdata/dmr_it_8.dis)", file=sys.stderr)
            return 2
        generated = f"/tmp/stream_load_gen_{args.port}_{int(args.rate)}.tmp"
        subprocess.run(
            [sys.executable, os.path.join(here, "make_test_bluefile.py"),
             args.bluefile, generated, "--rate", str(args.rate)],
            check=True)
        args.bluefile = generated
    elif not is_blue:
        print("error: input is not a BLUE file; if it is a 48 kHz discriminator "
              "capture, add --rate <sps> to generate test IQ from it",
              file=sys.stderr)
        return 2

    # Duration of the capture, for the stream-seconds denominator.
    info = subprocess.run(
        [sys.executable, client, args.bluefile, "--info"],
        capture_output=True, text=True, check=True).stdout
    n_samples = int(info.split(" complex samples")[0].rsplit(" ", 1)[1])
    rate = float(info.split("sample_rate=")[1].split(" ")[0])
    file_seconds = n_samples / rate

    env = dict(os.environ)
    if args.fme_path:
        env["PATH"] = args.fme_path + ":" + env.get("PATH", "")
    srv = subprocess.Popen(
        [args.server, "127.0.0.1", str(args.port), str(args.threads)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    time.sleep(1)
    if srv.poll() is not None:
        print(f"error: server exited immediately (code {srv.returncode})", file=sys.stderr)
        return 1

    cpu0, _ = tree_cpu_and_rss(srv.pid)
    t0 = time.time()
    wavs = [f"/tmp/stream_load_{args.port}_{i}.wav" for i in range(args.n)]
    clients = [subprocess.Popen(
        [sys.executable, client, args.bluefile, "--port", str(args.port),
         "--speed", "1", "--linger", "1", "--wav", wavs[i]],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for i in range(args.n)]
    time.sleep(min(file_seconds * 0.6, file_seconds - 1))
    _, rss_mid = tree_cpu_and_rss(srv.pid)
    for c in clients:
        c.wait()
    cpu1, _ = tree_cpu_and_rss(srv.pid)
    wall = time.time() - t0
    srv.terminate()
    srv.wait()

    cpu = cpu1 - cpu0
    stream_seconds = args.n * file_seconds
    per = 1000.0 * cpu / stream_seconds

    full = 0
    for path in wavs:
        try:
            w = wave.open(path)
            ch = max(w.getnchannels(), 1)
            if w.getnframes() / 8000.0 > 0.6 * file_seconds * (1 if ch else 1):
                full += 1
        except (FileNotFoundError, wave.Error):
            pass
        if not args.keep_wavs:
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass

    print(f"{os.path.basename(args.server)} x{args.n} realtime streams "
          f"({file_seconds:.1f} s file):")
    print(f"  server-tree CPU: {cpu:.2f} s over {stream_seconds:.0f} stream-seconds")
    print(f"  => {per:.1f} CPU-ms per stream-second ({per / 10:.2f}% of one core per stream)")
    print(f"  peak RSS at {args.n} sessions: {rss_mid / 1e6:.0f} MB "
          f"({rss_mid / 1e6 / args.n:.1f} MB/stream)")
    print(f"  {full}/{args.n} streams decoded full audio; wall {wall:.1f} s")
    cores = os.cpu_count() or 1
    print(f"  this machine ({cores} cores) could sustain ~"
          f"{int(cores * 1000 * 0.72 / per)} such streams at 72% utilization")
    return 0


if __name__ == "__main__":
    sys.exit(main())
