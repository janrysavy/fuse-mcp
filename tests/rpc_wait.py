#!/usr/bin/env python3
"""Real native RPC completion/timeout/race controls; no third-party packages.

Boot a generated uncompressed 48K snapshot containing only JR $. The component
does not write guest memory/registers through RPC. Run on either UI variant.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time


class RPC:
    def __init__(self,port):
        self.socket=socket.create_connection(('127.0.0.1',port),timeout=3)
        self.socket.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        self.stream=self.socket.makefile('rwb');self.serial=0
    def call(self,method,**params):
        self.serial+=1
        self.stream.write(json.dumps(dict(jsonrpc='2.0',id=self.serial,method=method,params=params)).encode()+b'\n')
        self.stream.flush();response=json.loads(self.stream.readline())
        if response.get('id')!=self.serial:raise AssertionError('Response ID mismatch')
        if 'error' in response:raise ValueError(response['error'])
        return response['result']
    def close(self):self.stream.close();self.socket.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    header=bytearray(30);header[6:10]=b'\x00\x80\x00\xff';header[29]=1
    memory=bytearray(49152);memory[16384:16386]=b'\x18\xfe'
    snapshot=out/'loop.z80';snapshot.write_bytes(header+memory)
    with socket.socket() as reservation:reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
    env=dict(os.environ,USERPROFILE=str(out),TEMP=str(out),TMP=str(out))
    command=[str(a.executable.resolve()),'--machine','48','--no-sound',
        '--no-autosave-settings','--no-confirm-actions','--speed','10000','--rpc-port',str(port),str(snapshot)]
    checks=[];clients=[];process=None;report=dict(passed=False,scope=__doc__,command=command,checks=checks)
    try:
        with (out/'emulator.log').open('wb') as log:
            process=subprocess.Popen(command,cwd=out,env=env,stdout=log,stderr=log,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            deadline=time.monotonic()+15
            while not clients:
                try:clients.append(RPC(port))
                except OSError:
                    if process.poll() is not None or time.monotonic()>deadline:raise
                    time.sleep(.02)
            control=clients[0];clients.extend([RPC(port),RPC(port)])
            initial=control.call('state.get')
            if initial['general']['pc']!=0x8000:raise AssertionError('Component did not start at loop')
            op=control.call('execution.continue')['operation_id']
            start=time.perf_counter();r=control.call('execution.wait',operation_id=op)
            if not r['running']:raise AssertionError('Zero wait unexpectedly stopped')
            checks.append(dict(name='legacy-nonblocking',seconds=time.perf_counter()-start))
            start=time.perf_counter();r=control.call('execution.wait',operation_id=op,timeout_ms=60)
            elapsed=time.perf_counter()-start
            if not r['running'] or not .045<=elapsed<2:raise AssertionError('Positive timeout ignored or excessive')
            checks.append(dict(name='bounded-running-timeout',seconds=elapsed))
            # Multiple waiting socket threads must leave the guest owner free
            # to service a separate pause request and wake every observer.
            barrier=threading.Barrier(3)
            def observer(client):
                barrier.wait();return client.call('execution.wait',operation_id=op,timeout_ms=1500)
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                futures=[executor.submit(observer,c) for c in clients[1:]]
                barrier.wait();time.sleep(.03)
                paused=control.call('execution.pause')
                answers=[f.result(timeout=3) for f in futures]
            if any(v['running'] or v['stop_reason']['registers']!=paused['stop_reason']['registers'] for v in answers):
                raise AssertionError('Concurrent waiters missed owner completion')
            checks.append(dict(name='concurrent-wake',observers=len(answers)))
            state=control.call('state.get');start=time.perf_counter()
            r=control.call('execution.wait',operation_id=op,timeout_ms=1500)
            if r['running'] or control.call('state.get')!=state or time.perf_counter()-start>=1:
                raise AssertionError('Completed wait delayed or advanced guest')
            checks.append(dict(name='completed-read-pure'))
            # Fast one-instruction operations repeatedly race queue delivery and
            # stop notification. Any lost wake would consume the 1500ms timeout.
            start=time.perf_counter()
            for _ in range(100):
                step=control.call('execution.step',mode='into')
                r=control.call('execution.wait',operation_id=step['operation_id'],timeout_ms=1500)
                if r['running'] or r['stop_reason']['kind']!='step':raise AssertionError('Fast completion lost')
            elapsed=time.perf_counter()-start
            if elapsed>=10:raise AssertionError('Repeated completion wakeups stalled')
            checks.append(dict(name='fast-completion-races',count=100,seconds=elapsed))
            for params in (dict(operation_id=op,timeout_ms=-1),dict(operation_id=op,timeout_ms=True),
                    dict(operation_id=op,timeout_ms=60001),dict(operation_id=2**64-1,timeout_ms=10)):
                try:control.call('execution.wait',**params)
                except ValueError:pass
                else:raise AssertionError('Invalid wait accepted')
            checks.append(dict(name='invalid-requests',count=4));report['passed']=True
    except Exception as error:report['error']=repr(error)
    finally:
        for client in clients:client.close()
        if process is not None and process.poll() is None:process.terminate();process.wait(timeout=5)
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('Native operation wait controls:',report['passed'],report.get('error'),flush=True)
    if not report['passed']:raise SystemExit(1)


if __name__=='__main__':main()
