"""MIT: compile verbatim production host functions with observable test dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiled',type=Path,required=True,help='Configured Fuse build directory')
    parser.add_argument('--prefix',type=Path,required=True,help='libspectrum installation prefix')
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(exist_ok=False)
    source=Path(__file__).resolve().parents[2]
    slt=(source/'slt.c').read_text();loader=(source/'loader.c').read_text()
    # Extract exact production functions and constants, never duplicate their bodies.
    functions=[slt[slt.index('int\nslt_trap('):slt.index('static void\nslt_reset(')],
        loader[loader.index('static void\nsoftware_projects_accelerate('):loader.index('static void\ndo_acceleration(')]]
    constants=re.findall(r'^#define (?:SOFTWARE_PROJECTS|GREMLIN)_[A-Z_]+ \d+$',loader,re.M)
    assert len(constants)==4
    (out/'host_functions.inc').write_text('\n'.join(constants+functions),newline='\n')
    tape=(source/'tape_traps.c').read_text()
    parts=[tape[tape.index('static void\nset_load_return_pc('):tape.index('static int\nfinish_trapped_load(')],tape[tape.index('int\ntape_load_trap('):tape.index('libspectrum_error\ntape_trap_finish_rom_block(')]]
    (out/'tape_functions.inc').write_text('\n'.join(parts),newline='\n')
    executable=out/('host-paths.exe' if os.name=='nt' else 'host-paths')
    command=[str(args.compiler.resolve()),'-std=c11','-DCORETEST','-Wall','-Wextra','-Werror',
        '-I',str(out),'-I',str(args.compiled.resolve()),'-I',str(source),
        '-I',str(args.prefix.resolve()/'include'),str(source/'tests/instruction-coverage/host_paths.c'),
        '-o',str(executable)]
    env=dict(os.environ);env['PATH']=str(args.compiler.resolve().parent)+os.pathsep+env['PATH']
    proof=dict(synthetic_component=True,campaign_certified=False,completed=False,passed=False,
        source_sha256={n:hashlib.sha256((source/n).read_bytes()).hexdigest() for n in
            ('slt.c','loader.c','tape_traps.c','instruction_coverage.h','tests/instruction-coverage/host_paths.c','tests/instruction-coverage/tape_faults.c','tests/instruction-coverage/host_paths.py')},
        command=command)
    try:
        subprocess.run(command,check=True,env=env,capture_output=True)
        result=subprocess.run([str(executable)],check=True,env=env,capture_output=True,text=True)
        command[-3]=str(source/'tests/instruction-coverage/tape_faults.c')
        command[-1]=str(out/('tape-faults.exe' if os.name=='nt' else 'tape-faults'))
        subprocess.run(command,check=True,env=env,capture_output=True)
        faults=subprocess.run([command[-1]],check=True,env=env,capture_output=True,text=True)
        proof.update(completed=True,passed=True,stdout=result.stdout+faults.stdout)
    except Exception as error:
        proof['failure']=repr(error);raise
    finally:(out/'report.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(proof["stdout"].strip())


if __name__=='__main__':main()
