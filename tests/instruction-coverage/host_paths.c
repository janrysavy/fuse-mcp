/* SPDX-License-Identifier: MIT. Synthetic host-path controls, not gameplay.
 * host_functions.inc contains verbatim production SLT/acceleration functions.
 * See the Python build recipe; CORETEST uses the observable memory callbacks.
 */
#include <assert.h>
#include <stdio.h>
#include "config.h"
#include "../../fuse_rpc.h"
#include "settings.h"
#include "../../loader_internals.h"
#include "../../z80/z80.h"
#include "../../memory_pages.h"
static libspectrum_byte *slt[256];
static size_t slt_length[256];
acceleration_mode_t acceleration_mode;
size_t acceleration_pc;
extern int tape_microphone;
#include "host_functions.inc"

zic_state *pyro_rpc_instruction_coverage;
settings_info settings_current;
processor z80;
int tape_microphone;
static unsigned reads, writes;
static unsigned char ram[65536];
libspectrum_byte readbyte_internal(libspectrum_word address) {
  ++reads; return ram[address];
}
void writebyte(libspectrum_word address, libspectrum_byte value) {
  ++writes; ram[address]=value;
}

static void begin(int enabled) {
  zic_reset(pyro_rpc_instruction_coverage);
  pyro_rpc_instruction_coverage->enabled=enabled;
  zic_begin(pyro_rpc_instruction_coverage,0x8000);
  zic_fetch(pyro_rpc_instruction_coverage,0x8000,0xdb,1);
  zic_fetch(pyro_rpc_instruction_coverage,0x8001,0xfe,0);
}

int main(void) {
  zic_state *s=pyro_rpc_instruction_coverage=zic_create(16);
  unsigned char data[]={0x12,0x34,0x56}; assert(s);
  slt[7]=data;slt_length[7]=sizeof(data);
  for (int enabled=0;enabled<=1;++enabled) {
    begin(enabled);writes=0;settings_current.slt_traps=0;
    slt_trap(0x9000,7);zic_finish(s,0x8002);
    assert(!writes && !s->semantic_override_units);
    begin(enabled);writes=0;settings_current.slt_traps=1;
    slt_trap(0x9000,8);zic_finish(s,0x8002);
    assert(!writes && !s->semantic_override_units);
    begin(enabled);writes=0;
    slt_trap(0x9000,7);zic_finish(s,0x8002);
    assert(writes==3 && !memcmp(ram+0x9000,data,3));
    assert(s->semantic_override_units==(unsigned)enabled);
    if(enabled) assert(s->rows[0].key.semantic_kinds==ZIC_SEMANTIC_SLT && zic_complete(s));
  }
  for(int mode=ACCELERATION_MODE_NONE;mode<=ACCELERATION_MODE_GREMLIN_FALLING;++mode)
    for(int long_pulse=0;long_pulse<=1;++long_pulse) {
      processor expected;unsigned expected_reads=0;
      for(int enabled=0;enabled<=1;++enabled) {
        memset(&z80,0,sizeof(z80));z80.pc.w=0x8002;z80.sp.w=0xf000;
        z80.af_.b.h=70;z80.hl.b.l=3;z80.bc.b.l=0x04;
        ram[0xf000]=0x34;ram[0xf001]=0x12;
        reads=0;acceleration_pc=0x8000;acceleration_mode=(acceleration_mode_t)mode;
        begin(enabled);accelerate_loader(long_pulse);zic_finish(s,z80.pc.w);
        if(!enabled){expected=z80;expected_reads=reads;}
        else assert(!memcmp(&expected,&z80,sizeof(z80)) && expected_reads==reads);
        assert(s->semantic_override_units==(unsigned)(enabled && mode!=ACCELERATION_MODE_NONE));
        if(enabled) assert(zic_complete(s));
      }
    }
  /* Host operations without a CPU dispatch never manufacture a unit. */
  zic_reset(s);settings_current.slt_traps=1;slt_trap(0x9000,7);
  acceleration_mode=ACCELERATION_MODE_INCREASING;accelerate_loader(0);
  assert(!s->units && !s->semantic_override_units);
  zic_destroy(s);
  puts("PASS: production SLT and all loader acceleration paths; off/on callbacks and state exact");
  return 0;
}
