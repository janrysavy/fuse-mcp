/* SPDX-License-Identifier: MIT. Fault injection in the verbatim tape-load
 * control function; dependencies are disposable fixtures, not real devices.
 * This certifies mutation-boundary tagging, not physical CPU/tape behavior.
 */
#include <assert.h>
#include <stdio.h>
#include "config.h"
#include "../../fuse_rpc.h"
#include "../../machine.h"
#include "../../z80/z80.h"
#include "../../z80/z80_macros.h"
zic_state *pyro_rpc_instruction_coverage;
processor z80;
static fuse_machine_info machine;
fuse_machine_info *machine_current=&machine;
libspectrum_tape *tape;
int trap_resume_pending;
static int mode, loaded, played, deactivated;
static int tape_trap_precheck(void){return mode==1;}
static int consume_pending_pause(libspectrum_tape_block **block){(void)block;return mode==2;}
static int skip_metadata_blocks(libspectrum_tape_block **block){(void)block;return mode==3;}
static int block_requires_playback(libspectrum_tape_block *block){(void)block;return mode==4;}
static void phantom_typist_deactivate(void){++deactivated;}
static int tape_play(int autoplay){(void)autoplay;++played;return 0;}
static libspectrum_tape_block *fixture_current_block(libspectrum_tape *value){
  (void)value;return (libspectrum_tape_block *)(uintptr_t)1;
}
#define libspectrum_tape_current_block fixture_current_block
static int tape_trap_load_block(libspectrum_tape_block *block,size_t *bytes){
  (void)block;++loaded;*bytes=3;z80.af.w=0x9940;return mode==5;
}
static int finish_trapped_load(size_t bytes){assert(bytes==3);return mode==6;}
#include "tape_functions.inc"

int main(void){
  zic_state *s=pyro_rpc_instruction_coverage=zic_create(16);assert(s);
  for(int timex=0;timex<=1;++timex)for(mode=0;mode<=6;++mode){
    processor expected;int expected_loaded=0,expected_played=0,expected_deactivated=0,expected_result=0;
    machine.machine=timex?LIBSPECTRUM_MACHINE_TC2068:LIBSPECTRUM_MACHINE_48;
    for(int enabled=0;enabled<=1;++enabled){
      zic_reset(s);s->enabled=enabled;memset(&z80,0,sizeof(z80));
      z80.pc.w=0x056c;z80.af.w=0x1240;loaded=played=deactivated=0;
      zic_begin(s,0x056b);zic_fetch(s,0x056b,0xc0,1);
      int result=tape_load_trap();
      /* The caller resumes its predicate on nonzero return. F has Z set. */
      if(result)zic_decision(s,!(z80.af.b.l&FLAG_Z));
      zic_finish(s,z80.pc.w);
      if(!enabled){expected=z80;expected_loaded=loaded;expected_played=played;
        expected_deactivated=deactivated;expected_result=result;}
      else assert(!memcmp(&z80,&expected,sizeof(z80)) && loaded==expected_loaded &&
        played==expected_played && deactivated==expected_deactivated && result==expected_result);
      int marked=(mode==0||mode==5||mode==6);
      assert(z80.pc.w==(marked?(timex?0x0136:0x05e2):0x056c));
      assert(s->semantic_override_units==(unsigned)(enabled&&marked));
      if(enabled){assert(zic_complete(s));assert(s->rows[0].key.outcome==(result?0:-1));
        assert(s->rows[0].key.semantic_kinds==(marked?ZIC_SEMANTIC_TAPE_LOAD:0));}
    }
  }
  zic_reset(s);mode=0;tape_load_trap();assert(!s->units&&!s->semantic_override_units);
  zic_destroy(s);puts("PASS: tape-load success, prechecks, playback and both post-mutation failures; ordinary/Timex");
  return 0;
}
