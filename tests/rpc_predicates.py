#!/usr/bin/env python3
"""Asset-free differential stop-predicate controls and bounded throughput probe.

Both executables boot the same generated 48K snapshot. Compare complete CPU,
mapped RAM, video and both coverage representations. No RPC guest-state writes.
The loop increments a two-byte counter in uncontended RAM with interrupts off.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from rpc_wait import RPC


def cpu_seconds(process):
    if os.name != 'nt':
        return None
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    query = kernel.GetProcessTimes
    query.argtypes = [ctypes.c_void_p] * 5
    query.restype = ctypes.c_int
    values = [ctypes.c_ulonglong() for _ in range(4)]
    if not query(int(process._handle), *(ctypes.byref(v) for v in values)):
        raise ctypes.WinError(ctypes.get_last_error())
    return (values[2].value + values[3].value) / 10_000_000


def run(executable, out, detailed):
    out.mkdir(exist_ok=False)
    header = bytearray(30)
    header[4:10] = bytes.fromhex('0090008000ff')
    header[29] = 1
    ram = bytearray(49152)
    ram[16384:16392] = bytes.fromhex('3420fd23342b18f8')
    snapshot = out / 'counter.z80'
    snapshot.write_bytes(header + ram)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    env = dict(os.environ, USERPROFILE=str(out), TEMP=str(out), TMP=str(out))
    command = [str(executable), '--machine', '48', '--no-sound',
               '--no-autosave-settings', '--no-confirm-actions', '--speed', '10000',
               '--rpc-port', str(port), str(snapshot)]
    e = None
    observations, timings = [], []
    with (out / 'emulator.log').open('wb') as log:
        process = subprocess.Popen(command, cwd=out, env=env, stdout=log, stderr=log,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            deadline = time.monotonic() + 15
            while e is None:
                try:
                    e = RPC(port)
                except OSError:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise
                    time.sleep(.02)
            e.socket.settimeout(60)
            assert e.call('state.get')['general']['pc'] == 0x8000
            e.call('coverage.start', reset=True)
            if detailed:
                e.call('coverage.detail.start', reset=True)

            def until(name, predicate, budget, expected):
                before_cpu = cpu_seconds(process)
                begin = time.perf_counter()
                op = e.call('execution.run_until', predicate=predicate,
                            max_emulated_ns=budget)['operation_id']
                answer = e.call('execution.wait', operation_id=op, timeout_ms=60000)
                elapsed = time.perf_counter() - begin
                after_cpu = cpu_seconds(process)
                assert not answer['running'], name
                assert answer['stop_reason']['kind'] == expected, (name, answer)
                timings.append(dict(name=name, host_seconds=elapsed,
                                    cpu_seconds=None if before_cpu is None else after_cpu-before_cpu))
                # Re-reading a completed operation must not advance state.
                state = e.call('state.get')
                assert e.call('execution.wait', operation_id=op, timeout_ms=1000) == answer
                assert e.call('state.get') == state
                detail = None
                if detailed:
                    detail = e.call('coverage.detail.read', limit=1024)
                    assert detail['complete'] and detail['next_offset'] == detail['unique_tuples']
                observations.append(dict(name=name, answer=answer, state=state,
                    memory=e.call('memory.read', address=0, length=65536),
                    video=e.call('video.snapshot'), pc=e.call('coverage.read'), detail=detail))

            mem = lambda address, value: dict(kind='memory', address=address, value=value)
            pc = lambda address: dict(kind='execution', address=address)
            until('initial-execution-match', pc(0x8000), 1, 'predicate')
            until('initial-memory-match', mem(0x9000, 0), 1, 'predicate')
            # The first INC takes longer than 1 ns; predicate wins over timeout.
            until('predicate-before-timeout', mem(0x9000, 1), 1, 'predicate')
            until('replace-memory-with-pc', pc(0x8000), 100000, 'predicate')
            rom = e.call('memory.read', address=0, length=1)['hex']
            until('rom-peek', mem(0, int(rom, 16)), 1, 'predicate')
            until('counter-high-byte', mem(0x9001, 0xff), 10000000000, 'predicate')
            # Stale compiled predicates must not survive stop/step/continue.
            op = e.call('execution.step', mode='into')['operation_id']
            step = e.call('execution.wait', operation_id=op, timeout_ms=1000)
            assert step['stop_reason']['kind'] == 'step'
            observations.append(dict(name='step-after-predicate', answer=step))
            for n in range(3):
                until('memory-timeout-'+str(n), mem(0x9002, 1), 10000000000, 'timeout')
                until('execution-timeout-'+str(n), pc(0x9002), 10000000000, 'timeout')

            invalid = [dict(kind='unknown', address=0), dict(kind='memory', address=-1, value=0),
                dict(kind='memory', address=65536, value=0), dict(kind='memory', address=0, value=256),
                dict(kind='memory', address=True, value=0), dict(kind='execution', address=0, value=0),
                dict(kind='execution'), dict(kind='memory', address=0), [], None]
            state = e.call('state.get')
            errors = []
            for p in invalid:
                try:
                    e.call('execution.run_until', predicate=p)
                except ValueError as error:
                    errors.append(error.args[0])
                else:
                    raise AssertionError('Invalid predicate accepted')
                assert e.call('state.get') == state
            observations.append(dict(name='invalid-parameters', errors=errors))
            return dict(observations=observations, timings=timings)
        finally:
            if e is not None:
                e.close()
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    report = dict(passed=False, test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  executables={k:hashlib.sha256(v.read_bytes()).hexdigest()
                               for k,v in [('baseline',args.baseline),('candidate',args.candidate)]}, runs=[])
    try:
        for detailed in (False, True):
            pair = {}
            for name, executable in [('baseline', args.baseline), ('candidate', args.candidate)]:
                result = run(executable.resolve(), out/(name+'-'+str(detailed)), detailed)
                pair[name] = result
                report['runs'].append(dict(name=name, detailed=detailed, **result))
            assert pair['baseline']['observations'] == pair['candidate']['observations'], 'Runtime behavior differs'
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
    finally:
        (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Predicate differential controls:', report['passed'], report.get('error'), flush=True)
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
