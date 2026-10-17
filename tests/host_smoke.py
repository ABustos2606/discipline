# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alejandro Bustos
"""Optional Linux host check, using a private dummy JACK server.

Needs jackd, jalv, Ingen, python3-jack-client and numpy. Never stops an
existing engine/server. Logs are kept in build/host-smoke/.
"""
import argparse
import os
from pathlib import Path
import re
import socket
import shutil
import subprocess
import sys
import time

import jack
import numpy as np

URI = "https://github.com/ABustos2606/discipline#"
PREFIXES = """@prefix patch: <http://lv2plug.in/ns/ext/patch#> .
@prefix ingen: <http://drobilla.net/ns/ingen#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(check, message):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise RuntimeError(message)


class Protocol:
    """Small request/response client for Ingen's documented patch protocol."""
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.connect(str(path))
        self.socket.settimeout(10)
        self.buffer = b""
        self.sequence = 0
        self.transcript = Path("build/host-smoke/protocol.log").open("w")

    def request(self, verb, fields, allow_error=False):
        self.sequence += 1
        message = (PREFIXES + f'[] a patch:{verb} ; patch:sequenceNumber '
                   f'"{self.sequence}"^^xsd:int ; {fields} .\n')
        self.socket.sendall(message.encode())
        self.transcript.write("SEND " + message + "\n")
        self.transcript.flush()
        deadline = time.monotonic() + 10
        while True:
            require(time.monotonic() < deadline, f"Ingen {verb} response timed out")
            while b"\0" not in self.buffer:
                remaining = deadline - time.monotonic()
                require(remaining > 0, f"Ingen {verb} response timed out")
                self.socket.settimeout(remaining)
                data = self.socket.recv(65536)
                require(data, "Ingen closed its socket")
                self.buffer += data
            data, self.buffer = self.buffer.split(b"\0", 1)
            text = data.decode()
            self.transcript.write("RECV " + text + "\n")
            self.transcript.flush()
            statuses = []
            for match in re.finditer(r"a patch:Response\s*;(.*?)\.\s*(?:\n|$)", text, re.S):
                response = match[1]
                sequence = re.search(r'patch:sequenceNumber "(\d+)"', response)
                status = re.search(r'patch:body "(-?\d+)"', response)
                if sequence and int(sequence[1]) == self.sequence and status:
                    statuses.append(int(status[1]))
            if statuses:
                self.last_status = max(statuses, key=abs)
                require(allow_error or self.last_status == 0, f"Ingen {verb}: {text}")
                return text

    def close(self):
        self.socket.close()
        self.transcript.close()

    def put(self, subject, body):
        return self.request("Put", f"patch:subject <{subject}> ; patch:body [ {body} ]")

    def connect(self, tail, head):
        return self.put("ingen:/main/", f"a ingen:Arc ; ingen:tail <{tail}> ; ingen:head <{head}>")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--ingen", default="ingen")
    parser.add_argument("--period", type=int, default=1024,
                        help="dummy JACK period (default: 1024, tolerant of WSL scheduling)")
    parser.add_argument("--clock-divider", action="store_true",
                        help="also test Peepo's PolyLV2 clock_divider (must be on LV2_PATH)")
    args = parser.parse_args()
    logdir = Path("build/host-smoke").resolve()
    logdir.mkdir(parents=True, exist_ok=True)
    server_name = f"discipline-core-{os.getpid()}"
    env = dict(os.environ, JACK_DEFAULT_SERVER=server_name, JACK_NO_AUDIO_RESERVATION="1")
    private_lv2 = logdir / f"lv2-{os.getpid()}"
    shutil.copytree(args.bundle.resolve(), private_lv2 / "discipline.lv2")
    env["LV2_PATH"] = str(private_lv2) + ":" + env.get("LV2_PATH", "/usr/local/lib/lv2:/usr/lib/lv2")
    processes, logs = [], []
    probe = None
    protocol = None

    def launch(name, command):
        log = (logdir / f"{name}.log").open("w")
        logs.append(log)
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env)
        processes.append(process)
        return process

    def stop(process):
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    try:
        server = launch("jack", ["jackd", "--no-realtime", "--timeout", "5000", "--name", server_name, "-d", "dummy", "-r", "48000", "-p", str(args.period)])
        wait_for(lambda: subprocess.run(["jack_lsp"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0, "private JACK server did not start")
        require(server.poll() is None, "JACK server exited")
        probe = jack.Client("discipline_probe", servername=server_name, no_start_server=True)
        sources = [probe.outports.register(side) for side in ("src_l", "src_r")]
        captures = [probe.inports.register(name) for name in ("cap_l", "cap_r", "send_l", "send_r")]
        cv_source = probe.outports.register("src_cv")
        observed = [0.0] * 4
        capture = np.zeros((4, 48000 * 4), dtype=np.float32)
        probe_frame = 0
        source = {"mode": "dc", "pulse": -1, "cv": 0.0}
        xruns = 0

        @probe.set_xrun_callback
        def xrun(delay):
            nonlocal xruns
            xruns += 1

        @probe.set_process_callback
        def process(frames):
            nonlocal probe_frame
            for port, level in zip(sources, (0.125, -0.25)):
                data = port.get_array()
                data[:] = level if source["mode"] == "dc" else 0
                if source["mode"] == "impulse" and probe_frame <= source["pulse"] < probe_frame + frames:
                    data[source["pulse"] - probe_frame] = level
            cv_source.get_array()[:] = source["cv"]
            for i, port in enumerate(captures):
                observed[i] = float(port.get_array().mean())
            offset = probe_frame % capture.shape[1]
            first = min(frames, capture.shape[1] - offset)
            for i, port in enumerate(captures):
                capture[i, offset:offset + first] = port.get_array()[:first]
                if first < frames:
                    capture[i, :frames - first] = port.get_array()[first:]
            probe_frame += frames

        probe.activate()

        def cable(tail, head):
            probe.connect(tail, head)

        for fragment in ("discipline", "return"):
            name = f"discipline_{fragment}"
            controls = ["-c", "size=50", "-c", "repeats=0", "-c", "level=0"] if fragment == "discipline" else []
            host = launch(f"jalv-{fragment}", ["jalv", "-i", "-p", "-n", name, "-x", *controls, URI + fragment])
            wait_for(lambda: bool(probe.get_ports(f"{name}:")), f"jalv did not load {fragment}")
            # JACK ports are registered before jalv activates its client.
            time.sleep(0.5)
            require(host.poll() is None, f"jalv {fragment} failed during activation")
            cable(sources[0], f"{name}:in_l")
            cable(sources[1], f"{name}:in_r")
            if fragment == "discipline":
                for symbol, destination in zip(("out_l", "out_r", "send_l", "send_r"), captures):
                    cable(f"{name}:{symbol}", destination)
                wait_for(lambda: observed == [0.125, -0.25, 0.125, -0.25], "jalv dry/Send echo mismatch")
            time.sleep(0.1)
            require(host.poll() is None, f"jalv {fragment} exited")
            stop(host)
            print(f"PASS jalv: {fragment} loads and runs", flush=True)

        for threads in (1, 4):
            path = Path(f"/tmp/{server_name}-{threads}.sock")
            engine = launch(f"ingen-{threads}", [args.ingen, "-e", "-f", "-s", server_name, "-n", f"discipline_ingen_{threads}", "-S", str(path), "-E", "0", "-p", str(threads)])
            wait_for(path.exists, "Ingen socket did not appear")
            # The control socket opens before the JACK driver activates.
            time.sleep(1)
            require(engine.poll() is None, "Ingen failed during JACK activation")
            protocol = Protocol(path)
            for side in ("l", "r"):
                protocol.put(f"ingen:/main/in_{side}", "a lv2:InputPort, lv2:AudioPort")
                protocol.put(f"ingen:/main/out_{side}", "a lv2:OutputPort, lv2:AudioPort")
            protocol.put("ingen:/main/clock_out", "a lv2:OutputPort, lv2:CVPort")
            protocol.put("ingen:/main/modify_in", "a lv2:InputPort, lv2:CVPort")
            for fragment in ("discipline", "return"):
                protocol.put(f"ingen:/main/{fragment}", f"a ingen:Block ; lv2:prototype <{URI}{fragment}>")

            def control(block, symbol, value):
                return protocol.request("Set", f'patch:subject <ingen:/main/{block}/{symbol}> ; '
                                        f'patch:property ingen:value ; patch:value "{value}"^^xsd:float')

            control("discipline", "size", 50)
            control("discipline", "repeats", 0)
            control("discipline", "level", 0)
            for side in ("l", "r"):
                protocol.put(f"ingen:/main/fx_{side}", "a ingen:Block ; lv2:prototype <http://lv2plug.in/plugins/eg-amp>")
                control(f"fx_{side}", "gain", -6.020599913)
            for side in ("l", "r"):
                protocol.connect(f"ingen:/main/in_{side}", f"ingen:/main/discipline/in_{side}")
                protocol.connect(f"ingen:/main/discipline/out_{side}", f"ingen:/main/out_{side}")
                protocol.connect(f"ingen:/main/discipline/send_{side}", f"ingen:/main/fx_{side}/in")
                protocol.connect(f"ingen:/main/fx_{side}/out", f"ingen:/main/return/in_{side}")
            protocol.connect("ingen:/main/discipline/clock", "ingen:/main/clock_out")
            protocol.connect("ingen:/main/modify_in", "ingen:/main/discipline/modify_cv")
            if args.clock_divider:
                protocol.put("ingen:/main/divider", "a ingen:Block ; lv2:prototype <http://polyeffects.com/lv2/basic_modular#clock_divider>")
                control("divider", "divider", 4)
                protocol.put("ingen:/main/divided_out", "a lv2:OutputPort, lv2:CVPort")
                protocol.connect("ingen:/main/discipline/clock", "ingen:/main/divider/in")
                protocol.connect("ingen:/main/divider/out", "ingen:/main/divided_out")
            # The Return's input-only shape must compile as a sink in the graph.
            text = protocol.request("Get", "patch:subject <ingen:/main/return>")
            require("in_l" in text and "in_r" in text and "paired" in text, "Ingen Return port discovery")
            client_name = f"discipline_ingen_{threads}"
            wait_for(lambda: bool(probe.get_ports(f"{client_name}:out_l")), "Ingen JACK outputs")
            for side, source_port, destination in zip(("l", "r"), sources, captures):
                cable(source_port, f"{client_name}:in_{side}")
                cable(f"{client_name}:out_{side}", destination)
            cable(f"{client_name}:clock_out", captures[2])
            cable(cv_source, f"{client_name}:modify_in")
            if args.clock_divider:
                cable(f"{client_name}:divided_out", captures[3])
            wait_for(lambda: observed[:2] == [0.125, -0.25], "Ingen passthrough mismatch")
            control("discipline", "level", 0.7071067812)
            control("discipline", "repeats", 0.485)

            def clock_check(period):
                for attempt in range(3):
                    time.sleep(0.3)
                    start, before = probe_frame, xruns
                    end = start + period * 12
                    wait_for(lambda: probe_frame >= end, "clock capture did not advance")
                    if xruns != before:
                        continue
                    take = capture[:, np.arange(start, end) % capture.shape[1]].copy()
                    rising = np.flatnonzero((take[2, 1:] > 0.5) & (take[2, :-1] <= 0.5)) + 1
                    require(len(rising) >= 10 and np.all(np.diff(rising) == period), "Size clock is sample-exact in Ingen")
                    for edge in rising[:-1]:
                        require(np.all(take[2, edge:edge + 48] == 1) and take[2, edge + 48] == 0, "clock is a 1 ms 0/1 trigger")
                    if args.clock_divider:
                        divided = np.flatnonzero((take[3, 1:] > 0.5) & (take[3, :-1] <= 0.5)) + 1
                        require(len(divided) >= 2 and np.all(np.diff(divided) == period * 4), "Peepo clock divider emits one edge per four Size triggers")
                        require(all(edge in rising for edge in divided), "divider edges line up with the Size clock")
                    print(f"PASS Ingen: Size clock{' through Peepo clock divider' if args.clock_divider else ''}, {period} frames, {threads} threads", flush=True)
                    return
                raise RuntimeError("WSL xruns prevented a clean clock capture")

            clock_check(2400)

            def impulse_check(delay=2400, decay=0.25):
                for attempt in range(3):
                    source["mode"] = "silence"
                    time.sleep(1)
                    start, before = probe_frame, xruns
                    source["pulse"] = start + args.period * 6 + 13
                    source["mode"] = "impulse"
                    end = source["pulse"] + delay * 3 + args.period * 3
                    wait_for(lambda: probe_frame >= end, "JACK capture did not advance")
                    if xruns != before:
                        print("NOTE host capture retried after a WSL scheduling xrun", flush=True)
                        continue
                    take = capture[:, np.arange(start, end) % capture.shape[1]].copy()
                    dry = np.flatnonzero(take[0] > 0.1)
                    require(len(dry) == 1, "expected one dry impulse")
                    first = int(dry[0]) + delay
                    require(first + delay < take.shape[1], "capture contains both echoes")
                    require(abs(float(take[0, first]) - 0.0625) < 1e-5, f"first echo time/level: {take[0, first]}")
                    require(abs(float(take[1, first]) + 0.125) < 1e-5, "right echo time/level")
                    require(abs(float(take[0, first + delay]) - 0.0625 * decay) < 1e-5, "repeat time/decay")
                    require(np.isfinite(take).all(), "finite engine output")
                    return
                raise RuntimeError("WSL xruns prevented a clean impulse capture")

            for routing in (0, 1, 2):
                control("discipline", "routing", routing)
                impulse_check(decay=0.5 if routing == 2 else 0.25)
                print(f"PASS Ingen: routing {routing}, exact 50 ms echoes through -6 dB modules, {threads} threads", flush=True)

            # Modify CV through Ingen: Smooth speed at +1x passes echoes; CV
            # -0.25 reaches noon, which stops them, leaving exactly the dry.
            source["mode"] = "dc"
            control("discipline", "modifier", 2)
            control("discipline", "modify", 0.75)
            wait_for(lambda: observed[0] > 0.2, "Smooth speed at +1x passes echoes")
            source["cv"] = -0.25
            wait_for(lambda: observed[0] == 0.125, "Modify CV moves Smooth speed to stop")
            source["cv"] = 0.0
            wait_for(lambda: observed[0] > 0.2, "echoes return when Modify CV is released")
            control("discipline", "modifier", 0)
            print(f"PASS Ingen: Modify CV drives the selected modifier, {threads} threads", flush=True)
            control("discipline", "sync", 1)
            control("discipline", "bpm", 300)
            control("discipline", "size_beats", 0.5)
            control("discipline", "repeats", 0)
            impulse_check(delay=4800, decay=0)
            clock_check(4800)
            control("discipline", "sync", 0)
            print(f"PASS Ingen: Size follows BPM/beats, {threads} threads", flush=True)

            # Exercise the real enabled port while Ingen continues calling run.
            source["mode"] = "dc"
            control("discipline", "wipe", 1)
            control("discipline", "memory_dry", 0)  # Wipe must override Off.
            wait_for(lambda: abs(observed[0] - 0.25) < 1e-6, "Wipe exposes memory dry")
            control("discipline", "dry_kill", 1)
            wait_for(lambda: abs(observed[0] - 0.125) < 1e-6, "Always dry kill removes live dry")
            control("discipline", "enabled", 0)
            wait_for(lambda: abs(observed[0]) < 1e-7, "port bypass respects Always dry kill")
            control("discipline", "dry_kill", 2)
            wait_for(lambda: abs(observed[0] - 0.125) < 1e-6, "Series dry kill passes bypass dry")
            control("discipline", "always", 1)
            control("discipline", "scan_mode", 1)
            control("discipline", "scan", 0.001)
            time.sleep(1)
            source["mode"] = "silence"
            control("discipline", "enabled", 1)
            wait_for(lambda: abs(observed[0] - 0.125) < 1e-5, "Always recorded while enabled port was zero")
            for symbol, value in (("scan", 0), ("scan_mode", 0), ("always", 0), ("dry_kill", 0),
                                  ("wipe", 0), ("memory_dry", 1)):
                control("discipline", symbol, value)
            source["mode"] = "dc"
            wait_for(lambda: abs(observed[0] - 0.1875) < 1e-5, "normal echo resumes after port bypass")
            print(f"PASS Ingen: Wipe, Memory dry, Dry kill and Always through enabled port, {threads} threads", flush=True)

            # A real filter can sit in the return path without a cable cycle.
            protocol.put("ingen:/main/filter", "a ingen:Block ; lv2:prototype <http://drobilla.net/plugins/fomp/mvclpf1>")
            control("filter", "freq", 2000)
            control("filter", "res", 0)
            protocol.request("Delete", "patch:body [ a ingen:Arc ; ingen:tail <ingen:/main/fx_l/out> ; ingen:head <ingen:/main/return/in_l> ]")
            protocol.connect("ingen:/main/fx_l/out", "ingen:/main/filter/in")
            protocol.connect("ingen:/main/filter/out", "ingen:/main/return/in_l")
            source["mode"] = "dc"
            wait_for(lambda: observed[0] > 0.13, "filter in return path produces an echo")
            protocol.request("Delete", "patch:subject <ingen:/main/filter>")
            protocol.connect("ingen:/main/fx_l/out", "ingen:/main/return/in_l")

            protocol.request("Put", "patch:subject <ingen:/main/> ; patch:body [ a ingen:Arc ; "
                             "ingen:tail <ingen:/main/fx_l/out> ; ingen:head <ingen:/main/discipline/in_l> ]", allow_error=True)
            require(protocol.last_status != 0, "Ingen rejects a cable feedback loop")
            require(engine.poll() is None, "engine survives feedback rejection")
            # Save controls while an OUT echo repeats forever; reloading must
            # restore the controls, while the captured audio starts empty.
            control("discipline", "routing", 2)
            control("discipline", "repeats", 1)
            control("discipline", "pair", 7)
            control("return", "pair", 7)
            control("discipline", "modifier", 3)
            control("discipline", "modify", 0.5)
            source["mode"] = "silence"
            time.sleep(0.25)
            source["pulse"] = probe_frame + args.period * 6 + 13
            source["mode"] = "impulse"
            time.sleep(0.6)
            save_path = logdir / f"{server_name}-{threads}.ingen"
            protocol.request("Copy", f"patch:subject <ingen:/main> ; patch:destination <{save_path.as_uri()}>")
            for block_name in ("discipline", "return"):
                protocol.request("Delete", f"patch:subject <ingen:/main/{block_name}>")
            protocol.request("Copy", f"patch:subject <{save_path.as_uri()}> ; patch:destination <ingen:/main/restored>")
            for side in ("l", "r"):
                protocol.connect(f"ingen:/main/in_{side}", f"ingen:/main/restored/in_{side}")
                protocol.connect(f"ingen:/main/restored/out_{side}", f"ingen:/main/out_{side}")
            for block_name, symbol, expected in (("discipline", "pair", 7), ("return", "pair", 7),
                                                  ("discipline", "size", 50), ("discipline", "repeats", 1),
                                                  ("discipline", "modifier", 3), ("discipline", "modify", 0.5)):
                text = protocol.request("Get", f"patch:subject <ingen:/main/restored/{block_name}/{symbol}>")
                values = re.findall(r'ingen:value "([0-9.eE+-]+)"', text)
                require(values and abs(float(values[-1]) - expected) < 1e-5, "saved control restores: " + symbol)
            time.sleep(0.3)
            require(abs(observed[0]) < 1e-7 and abs(observed[1]) < 1e-7, "reloaded audio buffers start empty")
            main_ttl = (save_path / "main.ttl").read_text()
            require("state:state" not in main_ttl, "audio buffers are not saved as LV2 state")
            source["mode"] = "dc"
            wait_for(lambda: observed[0] > 0.15, "reloaded pair produces echoes")
            print(f"PASS Ingen: saved controls/Pair restore, audio memory discarded, {threads} threads", flush=True)
            require(engine.poll() is None, "Ingen exited")
            protocol.close()
            protocol = None
            stop(engine)
            path.unlink(missing_ok=True)
            source["mode"] = "dc"
            print(f"PASS Ingen: input-only Return, filter path, feedback rejection, {threads} threads", flush=True)
    finally:
        if protocol:
            protocol.close()
        if probe:
            probe.close()
        for process in reversed(processes):
            if process.poll() is None:
                stop(process)
        for log in logs:
            log.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        sys.exit(f"FAIL host_smoke: {error} (see build/host-smoke/*.log)")
