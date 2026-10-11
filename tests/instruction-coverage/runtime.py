"""MIT: standalone, asset-free synthetic tape-shortcut RPC controls."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time


class RPC:
    def __init__(self, port):
        self.socket=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.file=self.socket.makefile('rwb');self.serial=0

    def call(self, method, **params):
        self.serial+=1
        self.file.write(json.dumps(dict(jsonrpc='2.0',id=self.serial,method=method,params=params)).encode()+b'\n')
        self.file.flush();response=json.loads(self.file.readline())
        assert response['id']==self.serial
        if 'error' in response:raise RuntimeError(response['error'])
        return response['result']

    def close(self):
        self.file.close();self.socket.close()


def run(executable, out, case, enabled):
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    command=[str(executable),'--machine','48','--no-sound','--no-autosave-settings',
             '--speed','10000','--rpc-port',str(port)]
    if case.endswith('-disabled'):command.append('--no-traps')
    if case.startswith('load') and case!='load-absent':command.append(str(out/'synthetic.tap'))
    env=dict(os.environ);env['PATH']=str(executable.parent)+os.pathsep+env['PATH']
    with (out/f'{case}-{enabled}.log').open('wb') as log:
        process=subprocess.Popen(command,env=env,stdout=log,stderr=log,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        e=None
        try:
            deadline=time.monotonic()+15
            while e is None:
                try:e=RPC(port)
                except OSError:
                    assert process.poll() is None and time.monotonic()<deadline
                    time.sleep(.02)
            e.call('execution.pause')
            regs=dict(pc=0x056b if case.startswith('load') else 0x04d0,
                      sp=0xf000,af=0x1234,af_alt=0xff01,bc=0x0201,de=3 if case.startswith('load') else 0,
                      hl=0xb000,ix=0x9000,iy=0xa000,i=0x80,r=0x80,iff1=0,iff2=0,im=1)
            if case=='load-wrong-flag':regs['af_alt']=0x0001
            state=e.call('state.get')
            e.call('state.set_registers',expected_state_revision=state['state_revision'],
                   expected={k:state['general'][k] for k in regs},set=regs)
            e.call('memory.write',address=0xf000,data_base64=base64.b64encode(bytes.fromhex('00a0')).decode())
            e.call('coverage.start',reset=True)
            if enabled:e.call('coverage.detail.start',reset=True)
            before=e.call('state.get')
            operation=e.call('execution.step')['operation_id']
            deadline=time.monotonic()+10
            while e.call('execution.wait',operation_id=operation)['running']:
                assert time.monotonic()<deadline;time.sleep(.002)
            after=e.call('state.get')
            observation=dict(before={k:v for k,v in before.items() if k!='state_revision'},
                after={k:v for k,v in after.items() if k!='state_revision'},
                ram=e.call('memory.read',address=0,length=65536)['sha256'],
                pc_map=e.call('coverage.read'),loaded=e.call('memory.read',address=0x9000,length=3)['data_base64'])
            detail=None
            if enabled:
                status=e.call('coverage.detail.status');page=e.call('coverage.detail.read',limit=256)
                assert status['schema']==2 and status['complete'] is True and status['dispatch_units']==1
                assert status['counter_overflow'] is False and status['opcode_override_failures']==0
                detail=dict(status=status,entries=page['entries'])
                kind=2 if case in ('load-success','load-wrong-flag') else 1 if case=='save-success' else 0
                assert status['semantic_override_units']==int(bool(kind))
                assert status['contains_semantic_overrides']==bool(kind)
                assert status['native_semantics_only']==(not kind)
                row=page['entries'][0]
                assert row['semantic_kinds']==kind and bool(row['flags']&8)==bool(kind)
                assert row['outcome']==(-1 if kind or case.startswith('save') else 1)
            return dict(observation=observation,detail=detail)
        finally:
            if e:e.close()
            if process.poll() is None:process.terminate()
            process.wait(timeout=5)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(exist_ok=False)
    executable=args.executable.resolve()
    payload=bytes.fromhex('ff123456');parity=0
    for byte in payload:parity^=byte
    payload+=bytes([parity]);(out/'synthetic.tap').write_bytes(len(payload).to_bytes(2,'little')+payload)
    proof=dict(schema=1,completed=False,passed=False,synthetic_component=True,campaign_certified=False,
        executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
        test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),cases=[])
    try:
        for case in ('save-success','save-disabled','load-success','load-wrong-flag','load-disabled','load-absent'):
            off=run(executable,out,case,False);on=run(executable,out,case,True)
            assert off['observation']==on['observation'],case
            if case=='save-success':assert on['observation']['after']['general']['pc']==0x053e
            if case=='load-success':
                assert on['observation']['after']['general']['pc']==0x05e2
                assert base64.b64decode(on['observation']['loaded'])==bytes.fromhex('123456')
            proof['cases'].append(dict(name=case,off=off,on=on))
        proof.update(completed=True,passed=True)
    except Exception as error:
        proof['failure']=repr(error);raise
    finally:(out/'report.json').write_text(json.dumps(proof,indent=2)+'\n')
    print('PASS: six production tape paths; off/on CPU, RAM and physical clocks exact')


if __name__=='__main__':main()
