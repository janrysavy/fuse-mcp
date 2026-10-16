#!/usr/bin/env python3
"""Native RPC tests for repeated LDxR/CPxR X/Y flags and terminal semantics.

All register/data setup is guest code in an initial synthetic snapshot. RPC
only observes, sets breakpoints and steps; it never writes memory/registers.
Exercise PC.13/PC.11 independently of ordinary A/data-derived flags. No ROM,
interrupt or physical-chip coverage claim is made by this component test.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from rpc_wait import RPC


def fixtures():
    cases=[]
    for base in (0x8000,0x8800,0xA000,0xA800):
        slot=0
        for name,opcode,increment in (('LDIR',0xB0,1),('LDDR',0xB8,-1),('CPIR',0xB1,1),('CPDR',0xB9,-1)):
            compare=name.startswith('CP')
            for mode in ('repeat','terminal','match') if compare else ('repeat','terminal'):
                for flags in (0,0xC1):
                    for accumulator in ((0,) if mode=='match' else (0x10,0x1A)) if compare else (0,2,8,10):
                        cases.append(dict(name=name,opcode=opcode,pc=base+slot*8,increment=increment,
                            compare=compare,mode=mode,flags=flags,a=accumulator,bc=1 if mode=='terminal' else 2))
                        slot+=1
    cases.append(dict(name='LDIR',opcode=0xB0,pc=0xFFFE,increment=1,
        compare=False,mode='repeat',flags=0,a=0,bc=2,wrap=True))
    return cases


def snapshot(cases):
    memory=bytearray(49152);cursor=0x6000
    def put(at,data):memory[at-0x4000:at-0x4000+len(data)]=data
    def word(n):return n.to_bytes(2,'little')
    for case in cases:
        src=0xD010;dst=0xE010;case.update(source=src,destination=dst,setup=cursor)
        # DI; LD SP; seed both potential destinations FF; set AF through a
        # guest PUSH/POP, then BC/HL/DE; JP the isolated block instruction.
        setup=(b'\xF3\x31'+word(0xF000)+b'\x3E\xFF\x32'+word(dst)+b'\x32'+word(dst+case['increment'])+
            b'\x01'+word(case['a']<<8|case['flags'])+b'\xC5\xF1\x01'+word(case['bc'])+
            b'\x21'+word(src)+b'\x11'+word(dst)+b'\xC3'+word(case['pc']))
        put(cursor,setup);cursor+=len(setup)
    if cursor>=0x8000:raise ValueError('Synthetic setup overlaps opcode regions')
    for i,case in enumerate(cases):
        target=cases[i+1]['setup'] if i+1<len(cases) else cursor
        put(case['pc'],bytes((0xED,case['opcode'])) if case.get('wrap') else
            bytes((0xED,case['opcode'],0xC3))+word(target))
    put(cursor,b'\x18\xFE')
    header=bytearray(30);header[6:8]=word(cases[0]['setup']);header[8:10]=word(0xF000);header[29]=1
    return bytes(header+memory)


def wait(rpc,operation):
    result=rpc.call('execution.wait',operation_id=operation['operation_id'],timeout_ms=20000)
    if result['running']:raise ValueError('Single CPU operation did not finish')
    return result


def expected_flags(case,remaining,repeat):
    if repeat:return ((case['flags']&1) if case['compare'] else (case['flags']&0xC1))|(
        2 if case['compare'] else 0)|4|((case['pc']>>8)&0x28)
    value=case['a'] # source byte is zero; compare cannot half-borrow.
    xy=(value&8)|((value&2)<<4)
    if not case['compare']:return (case['flags']&0xC1)|xy|(4 if remaining else 0)
    return (case['flags']&1)|2|xy|(value&128)|(64 if value==0 else 0)|(4 if remaining else 0)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    cases=fixtures();path=out/'block-repeat.z80';path.write_bytes(snapshot(cases))
    with socket.socket()as reservation:reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
    env=dict(os.environ,USERPROFILE=str(out),TMP=str(out),TEMP=str(out))
    command=[str(a.executable.resolve()),'--machine','48','--no-sound','--no-autosave-settings',
        '--no-confirm-actions','--speed','10000','--rpc-port',str(port),str(path)]
    report=dict(passed=False,cases=len(cases),checks=[],host_memory_register_writes=0,command=command)
    rpc=None;process=None
    with (out/'emulator.log').open('wb')as log:
        try:
            process=subprocess.Popen(command,cwd=out,env=env,stdout=log,stderr=log,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            deadline=time.monotonic()+15
            while rpc is None:
                try:rpc=RPC(port)
                except OSError:
                    if process.poll()is not None or time.monotonic()>deadline:raise
                    time.sleep(.02)
            for case in cases:
                bp=rpc.call('breakpoints.create',address=case['pc'])
                wait(rpc,rpc.call('execution.continue'));before=rpc.call('state.get')
                rpc.call('breakpoints.delete',breakpoint_id=bp['breakpoint_id'])
                if before['general']['pc']!=case['pc']:raise ValueError('Synthetic setup boundary missing')
                for iteration in range(2 if case['mode']=='repeat' else 1):
                    wait(rpc,rpc.call('execution.step',mode='into'));after=rpc.call('state.get')
                    remaining=case['bc']-iteration-1;repeat=remaining>0 and case['mode']!='match'
                    g=after['general'];wanted=expected_flags(case,remaining,repeat);d=case['increment']*(iteration+1)
                    observed=base64.b64decode(rpc.call('memory.read',address=case['destination']+case['increment']*iteration,length=1)['data_base64'])[0]
                    checks=dict(flags=(g['af']&255)==wanted,q=g['q']==wanted,a=g['af']>>8==case['a'],
                        pc=g['pc']==(case['pc']+(0 if repeat else 2))&65535,bc=g['bc']==remaining,
                        hl=g['hl']==case['source']+d,de=g['de']==case['destination']+(0 if case['compare'] else d),
                        data=observed==(255 if case['compare'] else 0),
                        clocks=after['clock']-before['clock']==(21 if repeat else 16),
                        refresh=(g['r']-before['general']['r'])&127==2,
                        memptr=g['memptr']==((case['pc']+1)&65535 if repeat else
                            (before['general']['memptr']+(case['increment'] if case['compare'] else 0))&65535))
                    report['checks'].append(dict(name=case['name'],mode=case['mode'],pc=case['pc'],
                        initial_f=case['flags'],a=case['a'],iteration=iteration,expected_f=wanted,
                        observed_f=g['af']&255,checks=checks,passed=all(checks.values())))
                    before=after
            report['passed']=all(row['passed']for row in report['checks'])
        except Exception as error:report['error']=repr(error)
        finally:
            if rpc:rpc.close()
            if process and process.poll()is None:process.terminate();process.wait(timeout=10)
            (out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    failed=[r for r in report['checks']if not r['passed']]
    print('Block repeat flags:',report['passed'],len(report['checks']),'iterations;',len(failed),'failed;',report.get('error'),flush=True)
    if not report['passed']:raise SystemExit(1)


if __name__=='__main__':main()
