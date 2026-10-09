// SPDX-License-Identifier: GPL-2.0-or-later
// Transport threads own sockets only. Fuse's main thread owns every guest access.
#include "config.h"
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"
#include "fuse_rpc.h"
extern "C" {
#include "libspectrum.h"
#include "fuse.h"
#include "spectrum.h"
#include "machine.h"
#include "z80/z80.h"
#include "keyboard.h"
#include "display.h"
#include "settings.h"
#include "sound.h"
#ifndef UI_NULL
#include "ui/ui.h"
#include "ui/uidisplay.h"
#endif
}
using J = nlohmann::json;
int pyro_rpc_enabled = 0;
int pyro_rpc_trace_enabled = 0;
int pyro_rpc_audio_enabled = 0;
struct AudioEdge { uint64_t clock; int mic,ear; };
static std::vector<AudioEdge> audio_edges;
static std::vector<short> audio_pcm;
static uint64_t audio_begin=0,audio_end=0,audio_pcm_origin=0,audio_max_frames=480000,audio_dropped=0;
static unsigned audio_rate=48000,audio_channels=1;
static int audio_mic=0,audio_ear=0,audio_initial_mic=0,audio_initial_ear=0;
static int port = 2304;
static bool paused = true;
static uint64_t revision = 1, next_op = 1, next_bp = 1, active_op = 0;
static uint64_t instructions = 0, step_target = 0, deadline = 0;
static int exempt_pc = -1;
static J predicate = nullptr, breakpoints = J::array(), completed = J::array();
static J last_stop = {{"kind", "startup"}};
struct TraceInstruction {
  uint64_t clock, end_clock;
  unsigned pc, next_pc, af, bc, de, hl, ix, iy, sp;
  std::vector<unsigned char> opcode;
};
struct TraceWrite { uint64_t clock; unsigned pc,address,before,value; };
static std::vector<TraceInstruction> trace_instructions;
static std::vector<TraceWrite> trace_writes;
static std::array<uint64_t,65536> trace_pc_counts{}, trace_pc_cycles{}, trace_pc_waits{};
static uint64_t trace_unit_waits=0;
static std::vector<std::pair<unsigned,unsigned>> trace_pc_ranges, trace_write_ranges;
static TraceInstruction trace_current{};
static bool trace_inflight=false, trace_event_pending=false;
static uint64_t trace_units=0, trace_events=0, trace_begin_clock=0,trace_end_clock=0,trace_dropped_writes=0;
static uint64_t trace_max_units=1000000, trace_max_records=100000, trace_max_writes=200000;
struct Request {
  std::string line, response;
  bool done = false;
  std::mutex mutex;
  std::condition_variable cv;
};
static std::mutex queue_mutex;
static std::deque<std::shared_ptr<Request>> requests;
static std::atomic<bool> pending{false};
static std::atomic<unsigned> client_count{0};
struct Invalid : std::runtime_error { using std::runtime_error::runtime_error; };
static void require(bool ok, const char *message) { if (!ok) throw Invalid(message); }
static uint64_t number(const J &v, uint64_t maximum, const char *message) {
  require(v.is_number_integer(), message);
  if (v.is_number_unsigned()) { auto n=v.get<uint64_t>(); require(n<=maximum,message); return n; }
  auto n=v.get<int64_t>(); require(n>=0 && uint64_t(n)<=maximum,message); return uint64_t(n);
}
static void fields(const J &p, std::initializer_list<const char *> allowed) {
  require(p.is_object(), "params must be an object");
  for (auto it=p.begin();it!=p.end();++it) {
    bool found=false; for (auto key:allowed) if (it.key()==key) found=true;
    require(found,"unknown parameter");
  }
}
static uint64_t clock_value() {
  return uint64_t(spectrum_get_frame_count())*machine_current->timings.tstates_per_frame+tstates;
}
static uint64_t time_ns() {
  auto clock=clock_value();auto frequency=machine_current->timings.processor_speed;
  return (clock/frequency)*1000000000ULL+(clock%frequency)*1000000000ULL/frequency;
}
static std::string hex(const std::vector<unsigned char> &v) {
  static const char *digits="0123456789abcdef"; std::string s; s.reserve(v.size()*2);
  for(auto b:v){s+=digits[b>>4];s+=digits[b&15];} return s;
}
static std::string hash(const std::vector<unsigned char> &v) {
  BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE h=nullptr; unsigned char digest[32];
  if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 provider failed");
  auto status=BCryptCreateHash(alg,&h,nullptr,0,nullptr,0,0);
  if(status>=0) status=BCryptHashData(h,const_cast<PUCHAR>(v.data()),ULONG(v.size()),0);
  if(status>=0) status=BCryptFinishHash(h,digest,32,0);
  if(h) BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg,0);
  if(status<0) throw std::runtime_error("SHA256 failed");
  return hex(std::vector<unsigned char>(digest,digest+32));
}
static const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64(const std::vector<unsigned char> &v) {
  std::string out; for(size_t i=0;i<v.size();i+=3) {
    unsigned n=unsigned(v[i])<<16; if(i+1<v.size())n|=unsigned(v[i+1])<<8; if(i+2<v.size())n|=v[i+2];
    out+=alphabet[(n>>18)&63];out+=alphabet[(n>>12)&63];
    out+=i+1<v.size()?alphabet[(n>>6)&63]:'=';out+=i+2<v.size()?alphabet[n&63]:'=';
  } return out;
}
static std::vector<unsigned char> unb64(const J &value) {
  require(value.is_string(),"data_base64 must be a string");auto s=value.get<std::string>();
  require(s.size()<=87384 && s.size()%4==0,"invalid base64 length");
  std::vector<unsigned char> v;
  for(size_t i=0;i<s.size();i+=4) {
    unsigned n=0;int pad=0;
    for(int j=0;j<4;j++) {
      char c=s[i+j]; if(c=='='){require(j>=2 && i+4==s.size(),"invalid base64 padding");pad++;n<<=6;}
      else {require(pad==0,"invalid base64 padding");auto x=alphabet.find(c);require(x!=std::string::npos,"invalid base64 character");n=(n<<6)|unsigned(x);}
    }
    require(pad<=2,"invalid base64 padding");v.push_back(n>>16);if(pad<2)v.push_back(n>>8);if(pad<1)v.push_back(n);
  }
  require(b64(v)==s,"noncanonical base64");return v;
}
static std::vector<unsigned char> read(unsigned address,unsigned length) {
  require(length>0 && address<=65535 && length<=65536-address,"memory range outside 64K");
  std::vector<unsigned char> v; v.reserve(length);
  for(unsigned a=address;a<address+length;a++) {
    auto &page=memory_map_read[a>>MEMORY_PAGE_SIZE_LOGARITHM];
    require(page.page && (page.source==memory_source_rom || page.source==memory_source_ram),"unmapped or peripheral memory refused");
    v.push_back(page.page[a&MEMORY_PAGE_SIZE_MASK]);
  }return v;
}
static J descriptor(unsigned a,unsigned n) {
  auto v=read(a,n);return {{"address",a},{"length",n},{"hex",hex(v)},{"data_base64",b64(v)},{"sha256",hash(v)},{"state_revision",revision}};
}
// PNG RGBA8, filter None. Compression is supplied by our pinned libspectrum.
// This observes guest/display state on Fuse's main thread; it never runs the CPU.
static void check_paused();
static void png_be32(std::vector<unsigned char> &v, uint32_t n) {
  for(int shift=24;shift>=0;shift-=8)v.push_back(static_cast<unsigned char>(n>>shift));
}
static void png_chunk(std::vector<unsigned char> &v,const char *type,const std::vector<unsigned char> &data) {
  png_be32(v,uint32_t(data.size()));size_t start=v.size();
  v.insert(v.end(),type,type+4);v.insert(v.end(),data.begin(),data.end());
  uint32_t crc=0xffffffffu;
  for(size_t i=start;i<v.size();i++) {
    crc^=v[i];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
  }
  png_be32(v,crc^0xffffffffu);
}
static J export_png(const J &p) {
  fields(p,{"path","source","flash_phase"});check_paused();
  std::string source="screen_ram";
  if(p.contains("source")){require(p["source"].is_string(),"source must be a string");source=p["source"].get<std::string>();}
  require(source=="screen_ram" || source=="ula_raster","invalid PNG source");
  require(source=="screen_ram" || !p.contains("flash_phase"),"FLASH override only applies to screen_ram");
  unsigned phase=p.contains("flash_phase")?unsigned(number(p["flash_phase"],1,"invalid flash_phase")):
    ((spectrum_get_frame_count()/DISPLAY_FLASH_HALF_PERIOD)&1u);
  auto screen=read(0x4000,6912);
  std::vector<unsigned char> rgba,filtered;rgba.reserve(256*192*4);filtered.reserve(192*(1+256*4));
  for(unsigned y=0;y<192;y++) {
    filtered.push_back(0);
    for(unsigned x=0;x<256;x++) {
      unsigned colour;
      if(source=="ula_raster")colour=unsigned(display_getpixel(int(x)+DISPLAY_BORDER_ASPECT_WIDTH,int(y)+DISPLAY_BORDER_HEIGHT));
      else {
        unsigned attr=screen[6144+(y/8)*32+x/8];
        unsigned ink=(attr&7)|((attr&64)>>3),paper=((attr>>3)&7)|((attr&64)>>3);
        if((attr&128) && phase)std::swap(ink,paper);
        unsigned address=((y&0xc0)<<5)+((y&7)<<8)+((y&0x38)<<2)+x/8;
        colour=(screen[address]&(128>>(x%8)))?ink:paper;
      }
      // Same palette as Fuse screenshot.c and automation/artifacts.c: 192/255.
      unsigned level=(colour&8)?255:192;
      std::array<unsigned char,4> pixel={static_cast<unsigned char>((colour&2)?level:0),
        static_cast<unsigned char>((colour&4)?level:0),static_cast<unsigned char>((colour&1)?level:0),255};
      rgba.insert(rgba.end(),pixel.begin(),pixel.end());filtered.insert(filtered.end(),pixel.begin(),pixel.end());
    }
  }
  libspectrum_byte *compressed=nullptr;size_t compressed_size=0;
  if(libspectrum_zlib_compress(filtered.data(),filtered.size(),&compressed,&compressed_size)!=LIBSPECTRUM_ERROR_NONE)
    throw std::runtime_error("PNG compression failed");
  std::vector<unsigned char> png={137,80,78,71,13,10,26,10},ihdr;
  png_be32(ihdr,256);png_be32(ihdr,192);ihdr.insert(ihdr.end(),{8,6,0,0,0});
  png_chunk(png,"IHDR",ihdr);png_chunk(png,"IDAT",std::vector<unsigned char>(compressed,compressed+compressed_size));
  libspectrum_free(compressed);png_chunk(png,"IEND",{});
  J result={{"available",true},{"format","png"},{"width",256},{"height",192},{"projection","native"},
    {"source",source},{"palette","fuse_rgb_192_255"},{"byte_size",png.size()},{"sha256",hash(png)},
    {"rgba_sha256",hash(rgba)},{"source_screen_ram_sha256",hash(screen)},{"clock",clock_value()},
    {"state_revision",revision},{"frame_count",spectrum_get_frame_count()}};
  if(source=="screen_ram")result["flash_phase"]=phase;
  if(p.contains("path")) {
    require(p["path"].is_string(),"path must be a string");auto path=p["path"].get<std::string>();
    require(!path.empty() && path.find('\0')==std::string::npos,"invalid path");
    std::ofstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::trunc);
    file.write(reinterpret_cast<const char *>(png.data()),std::streamsize(png.size()));file.close();
    if(!file)throw std::runtime_error("Cannot write PNG file");result["path"]=path;
  } else result["data_base64"]=b64(png);
  return result;
}
static J registers() {
  J g={{"af",z80.af.w},{"bc",z80.bc.w},{"de",z80.de.w},{"hl",z80.hl.w},
    {"af_alt",z80.af_.w},{"bc_alt",z80.bc_.w},{"de_alt",z80.de_.w},{"hl_alt",z80.hl_.w},
    {"ix",z80.ix.w},{"iy",z80.iy.w},{"sp",z80.sp.w},{"pc",z80.pc.w},{"i",z80.i},
    {"r",(z80.r&127)|(z80.r7&128)},{"iff1",z80.iff1},{"iff2",z80.iff2},{"im",z80.im},{"memptr",z80.memptr.w},{"q",z80.q}};
  std::string flag_text="SZ5H3PNC";
  for(unsigned i=0;i<8;i++)if(!(z80.af.b.l&(128>>i)))flag_text[i]='-';
  return {{"general",g},{"ip",z80.pc.w},{"flags",z80.af.b.l},{"flags_text",flag_text},{"clock",clock_value()},
    {"emulated_time_ns",time_ns()},{"in_hlt",bool(z80.halted)},{"state_revision",revision}};
}
static void check_paused(){require(paused,"machine must be paused");}
static void guard_revision(const J&p){require(p.contains("expected_state_revision") && number(p.at("expected_state_revision"),UINT64_MAX,"invalid revision")==revision,"stale or missing expected_state_revision");}
static void stop(const std::string &kind, J detail=J::object()) {
  paused=true;step_target=deadline=0;predicate=nullptr;revision++;
  detail["kind"]=kind;detail["registers"]=registers();last_stop=detail;
  if(active_op) {
    completed.push_back({{"operation_id",active_op},{"state","stopped"},{"running",false},{"paused",true},{"stop_reason",last_stop}});
    if(completed.size()>64) completed.erase(completed.begin()); active_op=0;
  }
}
static J start(bool stepping) {
  auto r=registers();active_op=next_op++;paused=false;revision++;
  step_target=stepping?instructions+1:0;
  r["operation_id"]=active_op;r["state"]="running";r["paused"]=false;r["stepping"]=stepping;r["state_revision"]=revision;
  return r;
}
static J capabilities() {
  return {{"emulator","Fuse"},{"version",PACKAGE_VERSION},{"session_id","fuse"},{"cpu","Z80"},{"model",machine_current->id},
    {"native_debugger",true},{"transport","JSON-RPC 2.0 TCP JSON-lines"},{"bind","127.0.0.1"},{"rpc_port",port},
    {"cpu_cycle_frequency",machine_current->timings.processor_speed},{"time_base","native ULA frames and Z80 T-states"},
    {"methods",{"agent.capabilities","emulator.info","session.status","state.get","state.get_registers","state.observe","state.set_registers","memory.read","memory.write","execution.pause","execution.continue","execution.go","execution.step","execution.run_until","execution.wait","breakpoints.create","breakpoints.list","breakpoints.delete","input.keyboard","video.snapshot","video.export_png","trace.start","trace.stop","trace.status","trace.read","audio.start","audio.stop","audio.status","audio.read"}},
    {"limits",{{"address_bits",16},{"max_line_bytes",1048576},{"max_queue",64},{"max_breakpoints",256},{"max_memory_bytes",65536}}},
    {"unsupported",{"watchpoints","conditional_breakpoints","step_over","reverse_execution","complete_machine_snapshots","GUI","XT_scan_codes"}}};
}
static bool trace_contains(const std::vector<std::pair<unsigned,unsigned>>&ranges,unsigned address) {
  for(auto range:ranges)if(address>=range.first&&address<range.second)return true;
  return false;
}
static J trace_status() {
  return {{"active",bool(pyro_rpc_trace_enabled)},{"units",trace_units},{"instruction_records",trace_instructions.size()},
    {"write_records",trace_writes.size()},{"dropped_writes",trace_dropped_writes},
    {"begin_clock",trace_begin_clock},{"end_clock",pyro_rpc_trace_enabled?clock_value():trace_end_clock},
    {"event_tstates",trace_events},{"max_units",trace_max_units},{"max_records",trace_max_records},{"max_writes",trace_max_writes},
    {"timing","native opcode/refresh units; device-event cycles counted separately"}};
}
static std::vector<std::pair<unsigned,unsigned>> trace_ranges(const J&value) {
  require(value.is_array()&&value.size()<=16,"need at most 16 ranges");
  std::vector<std::pair<unsigned,unsigned>> result;
  for(auto&r:value){fields(r,{"address","length"});unsigned a=number(r.at("address"),65535,"invalid trace range");
    unsigned n=number(r.at("length"),65536,"invalid trace length");require(n>0&&a+n<=65536,"trace range outside 64K");result.emplace_back(a,a+n);}
  return result;
}
static void trace_begin_unit() {
  if(!pyro_rpc_trace_enabled)return;
  trace_current={clock_value(),0,z80.pc.w,0,z80.af.w,z80.bc.w,z80.de.w,z80.hl.w,z80.ix.w,z80.iy.w,z80.sp.w,{}};
  if(trace_contains(trace_pc_ranges,z80.pc.w))trace_current.opcode=read(z80.pc.w,std::min(4u,65536u-z80.pc.w));
  trace_inflight=true;
}
extern "C" void pyro_rpc_memory_write(unsigned address,unsigned before,unsigned value) {
  if(!pyro_rpc_trace_enabled||!trace_inflight||!trace_contains(trace_write_ranges,address))return;
  if(trace_writes.size()>=trace_max_writes){trace_dropped_writes++;return;}
  trace_writes.push_back({clock_value(),trace_current.pc,address,before,value});
}
static void set_register(const std::string &key,unsigned v) {
  if(key=="af")z80.af.w=v;else if(key=="bc")z80.bc.w=v;else if(key=="de")z80.de.w=v;else if(key=="hl")z80.hl.w=v;
  else if(key=="af_alt")z80.af_.w=v;else if(key=="bc_alt")z80.bc_.w=v;else if(key=="de_alt")z80.de_.w=v;else if(key=="hl_alt")z80.hl_.w=v;
  else if(key=="ix")z80.ix.w=v;else if(key=="iy")z80.iy.w=v;else if(key=="sp")z80.sp.w=v;
  else if(key=="pc") {
    // A debugger redirect resumes at the new instruction. Retaining HALT would
    // make Fuse's next interrupt push PC+1 and skip an unrelated instruction.
    // Setting the same PC (or changing other registers) retains HALT state.
    if(z80.pc.w!=v) z80.halted=0;
    z80.pc.w=v;
  }
  else if(key=="i")z80.i=v;else if(key=="r"){z80.r=v&127;z80.r7=v&128;}
  else if(key=="iff1")z80.iff1=v;else if(key=="iff2")z80.iff2=v;else if(key=="im")z80.im=v;
}
static void validate_predicate(const J&p) {
  require(p.is_object() && p.contains("kind") && p.at("kind").is_string(),"predicate needs kind");
  auto kind=p.at("kind").get<std::string>();
  if(kind=="execution"){fields(p,{"kind","address"});number(p.at("address"),65535,"invalid PC address");}
  else if(kind=="memory"){fields(p,{"kind","address","value"});number(p.at("address"),65535,"invalid memory address");number(p.at("value"),255,"invalid byte");}
  else throw Invalid("unsupported predicate kind");
}
static bool matches(const J &p) {
  auto a=p.at("address").get<unsigned>();
  return p.at("kind")=="execution" ? z80.pc.w==a : read(a,1)[0]==p.at("value").get<unsigned>();
}
static J dispatch(const std::string &method,const J &p) {
  auto audio_status=[](){return J{{"active",bool(pyro_rpc_audio_enabled)},
    {"begin_clock",audio_begin},{"end_clock",pyro_rpc_audio_enabled?clock_value():audio_end},
    {"clock_hz",machine_current->timings.processor_speed},{"sample_rate",audio_rate},
    {"channels",audio_channels},{"pcm_frames",audio_pcm.size()/audio_channels},
    {"pcm_origin_clock",audio_pcm_origin},{"edge_records",audio_edges.size()},
    {"initial_mic",audio_initial_mic},{"initial_ear",audio_initial_ear},
    {"dropped_records",audio_dropped},{"max_frames",audio_max_frames},
    {"pcm_model","Native Fuse output_mixer/speaker filters, fresh filter history, complete raster frames only"}};};
  if(method=="audio.start") {
    fields(p,{"sample_rate","max_frames"});check_paused();
    require(!pyro_rpc_audio_enabled,"audio capture already active");
    unsigned rate=number(p.value("sample_rate",J(48000)),192000,"invalid audio sample rate");
    require(rate>=8000,"audio sample rate too low");
    uint64_t frames=number(p.value("max_frames",J(480000)),1920000,"audio frame limit too high");
    require(frames>0,"audio frame limit must be positive");
    sound_end();audio_edges.clear();audio_pcm.clear();audio_dropped=0;
    audio_rate=rate;audio_channels=1;audio_max_frames=frames;
    audio_begin=audio_end=clock_value();audio_pcm_origin=audio_begin-tstates;
    audio_initial_mic=audio_mic;audio_initial_ear=audio_ear;
    settings_current.sound=1;settings_current.sound_freq=rate;
    pyro_rpc_audio_enabled=1;sound_init(nullptr);
    if(!sound_enabled){pyro_rpc_audio_enabled=0;throw Invalid("Native headless sound initialization failed");}
    return audio_status();
  }
  if(method=="audio.stop") {fields(p,{});check_paused();require(pyro_rpc_audio_enabled,"audio capture inactive");
    audio_end=clock_value();pyro_rpc_audio_enabled=0;sound_end();return audio_status();}
  if(method=="audio.status"){fields(p,{});check_paused();return audio_status();}
  if(method=="audio.read") {
    fields(p,{"section","offset","limit"});check_paused();
    require(p.contains("section")&&p.at("section").is_string(),"audio section required");
    auto section=p.at("section").get<std::string>();
    uint64_t offset=number(p.value("offset",J(0)),2000000,"audio offset too high");
    uint64_t limit=number(p.value("limit",J(2048)),4096,"audio page too large");require(limit>0,"empty audio page");
    J result={{"section",section},{"offset",offset},{"status",audio_status()}};
    if(section=="pcm") {
      uint64_t total=audio_pcm.size()/audio_channels;
      unsigned n=offset>=total?0:std::min(limit,total-offset);
      std::vector<unsigned char> bytes;bytes.reserve(n*audio_channels*2);
      for(size_t i=offset*audio_channels;i<(offset+n)*audio_channels;i++) {
        unsigned v=unsigned(uint16_t(audio_pcm[i]));bytes.push_back(v&255);bytes.push_back(v>>8);
      }
      result.update({{"total",total},{"next_offset",offset+n},{"data_base64",b64(bytes)},{"format","s16le"}});
    } else if(section=="events") {
      J rows=J::array();for(size_t i=offset;i<audio_edges.size()&&rows.size()<limit;i++) {
        auto &e=audio_edges[i];rows.push_back({{"clock",e.clock},{"mic",e.mic},{"ear",e.ear}});
      }
      result.update({{"total",audio_edges.size()},{"next_offset",offset+rows.size()},{"records",rows}});
    } else throw Invalid("unknown audio section");
    return result;
  }
  if(method=="trace.start") {
    fields(p,{"pc_ranges","write_ranges","max_units","max_records","max_writes"});check_paused();
    auto pcs=trace_ranges(p.value("pc_ranges",J::array({{{"address",0},{"length",65536}}})));
    auto writes=trace_ranges(p.value("write_ranges",J::array()));
    auto units=number(p.value("max_units",J(1000000)),10000000,"trace unit limit too large");
    auto records=number(p.value("max_records",J(100000)),250000,"trace record limit too large");
    auto write_count=number(p.value("max_writes",J(200000)),500000,"trace write limit too large");
    require(units>0&&records>0&&write_count>0,"trace limits must be positive");
    std::vector<TraceInstruction> ins;ins.reserve(records);std::vector<TraceWrite> wr;wr.reserve(write_count);
    trace_instructions=std::move(ins);trace_writes=std::move(wr);trace_pc_ranges=std::move(pcs);trace_write_ranges=std::move(writes);
    trace_pc_counts.fill(0);trace_pc_cycles.fill(0);trace_pc_waits.fill(0);trace_unit_waits=0;trace_units=trace_events=trace_dropped_writes=0;
    trace_max_units=units;trace_max_records=records;trace_max_writes=write_count;trace_inflight=trace_event_pending=false;
    trace_begin_clock=trace_end_clock=clock_value();pyro_rpc_trace_enabled=1;return trace_status();
  }
  if(method=="trace.stop") {fields(p,{});check_paused();pyro_rpc_trace_enabled=0;trace_end_clock=clock_value();return trace_status();}
  if(method=="trace.status") {fields(p,{});return trace_status();}
  if(method=="trace.read") {
    fields(p,{"section","offset","limit"});check_paused();
    auto section=p.value("section",J("instructions")).get<std::string>();
    auto offset=number(p.value("offset",J(0)),500000,"trace offset too large");
    auto limit=number(p.value("limit",J(1024)),2048,"trace page too large");require(limit>0,"trace limit must be positive");
    J rows=J::array();uint64_t total=0;
    if(section=="instructions") {
      total=trace_instructions.size();for(size_t i=offset;i<total&&rows.size()<limit;i++){auto&r=trace_instructions[i];
        rows.push_back({{"clock",r.clock},{"end_clock",r.end_clock},{"pc",r.pc},{"next_pc",r.next_pc},
          {"af",r.af},{"bc",r.bc},{"de",r.de},{"hl",r.hl},{"ix",r.ix},{"iy",r.iy},{"sp",r.sp},{"opcode_prefix_hex",hex(r.opcode)}});}
    } else if(section=="writes") {
      total=trace_writes.size();for(size_t i=offset;i<total&&rows.size()<limit;i++){auto&r=trace_writes[i];
        rows.push_back({{"clock",r.clock},{"pc",r.pc},{"address",r.address},{"before",r.before},{"value",r.value}});}
    } else if(section=="profile") {
      for(unsigned pc=0;pc<65536;pc++)if(trace_pc_counts[pc]){if(total>=offset&&rows.size()<limit)rows.push_back({{"pc",pc},{"count",trace_pc_counts[pc]},{"tstates",trace_pc_cycles[pc]},{"wait_tstates",trace_pc_waits[pc]}});total++;}
    } else throw Invalid("unknown trace section");
    return {{"section",section},{"offset",offset},{"total",total},{"next_offset",offset+rows.size()},{"records",rows},{"status",trace_status()}};
  }
  if(method=="agent.capabilities" || method=="emulator.info"){fields(p,{});return capabilities();}
  if(method=="session.status") {
    fields(p,{"session_id"});if(p.contains("session_id"))require(p.at("session_id")=="fuse","unknown session");
    return {{"session_id","fuse"},{"state",paused?"paused":"running"},{"paused",paused},{"state_revision",revision},{"clock",clock_value()},{"emulated_time_ns",time_ns()},{"last_stop",last_stop}};
  }
  if(method=="state.get" || method=="state.get_registers"){fields(p,{});return registers();}
  if(method=="memory.read"){fields(p,{"address","length"});return descriptor(number(p.at("address"),65535,"invalid address"),number(p.value("length",J(1)),65536,"invalid length"));}
  if(method=="video.snapshot"){fields(p,{});auto v=descriptor(16384,6912);v["adapter"]="ZX Spectrum ULA";v["width"]=256;v["height"]=192;v["format"]="spectrum-screen";return v;}
#ifndef UI_NULL
  if(method=="video.present") {
    fields(p,{"flash_phase"});check_paused();
    unsigned phase=p.contains("flash_phase")?unsigned(number(p["flash_phase"],1,"invalid flash_phase")):
      ((spectrum_get_frame_count()/DISPLAY_FLASH_HALF_PERIOD)&1u);
    auto screen=read(0x4000,6912);
    for(unsigned y=0;y<192;y++)for(unsigned x=0;x<32;x++) {
      unsigned attr=screen[6144+(y/8)*32+x];
      unsigned ink=(attr&7)|((attr&64)>>3),paper=((attr>>3)&7)|((attr&64)>>3);
      if((attr&128)&&phase)std::swap(ink,paper);
      unsigned address=((y&0xc0)<<5)+((y&7)<<8)+((y&0x38)<<2)+x;
      uidisplay_plot8(x+DISPLAY_BORDER_ASPECT_WIDTH/8,y+DISPLAY_BORDER_HEIGHT,screen[address],ink,paper);
    }
    uidisplay_area(0,0,DISPLAY_ASPECT_WIDTH,DISPLAY_SCREEN_HEIGHT);uidisplay_frame_end();
    return {{"source","screen_ram"},{"clock",clock_value()},{"state_revision",revision}};
  }
#endif
  if(method=="video.export_png")return export_png(p);
  if(method=="state.observe") {
    fields(p,{"expected_state_revision","memory","video_memory"});check_paused();guard_revision(p);
    auto windows=p.value("memory",J::array());require(windows.is_array()&&windows.size()<=16,"invalid memory windows");
    require(!p.contains("video_memory") || p.at("video_memory").is_boolean(),"video_memory must be boolean");
    unsigned total=0;std::vector<std::pair<unsigned,unsigned>> ranges;
    for(auto&w:windows){fields(w,{"address","length"});auto a=number(w.at("address"),65535,"invalid address");auto n=number(w.value("length",J(1)),65536,"invalid length");require(n>0 && a+n<=65536,"invalid memory range");total+=n;require(total<=65536,"too much observation memory");ranges.emplace_back(a,n);}
    J r={{"state_revision",revision},{"registers",registers()},{"memory",J::array()}};
    for(auto range:ranges)r["memory"].push_back(descriptor(range.first,range.second));
    if(p.value("video_memory",false)){r["video_memory"]=descriptor(16384,6912);r["video_memory"]["adapter"]="ZX Spectrum ULA";}
    return r;
  }
  if(method=="memory.write") {
    fields(p,{"address","data_base64","expected_sha256"});check_paused();auto a=number(p.at("address"),65535,"invalid address");auto v=unb64(p.at("data_base64"));
    require(!v.empty() && v.size()<=65536-a,"invalid write range");
    for(unsigned x=a;x<a+v.size();x++){auto&page=memory_map_write[x>>MEMORY_PAGE_SIZE_LOGARITHM];require(page.page && page.writable && page.source==memory_source_ram,"write includes ROM or peripheral memory");}
    auto before=hash(read(a,v.size()));if(p.contains("expected_sha256")){require(p.at("expected_sha256").is_string(),"invalid hash");auto h=p.at("expected_sha256").get<std::string>();std::transform(h.begin(),h.end(),h.begin(),[](unsigned char c){return char(std::tolower(c));});require(h==before,"memory hash mismatch");}
    for(unsigned i=0;i<v.size();i++)writebyte_internal(a+i,v[i]);revision++;
    return {{"address",a},{"length",v.size()},{"before_sha256",before},{"after_sha256",hash(read(a,v.size()))},{"state_revision",revision}};
  }
  if(method=="state.set_registers") {
    fields(p,{"expected_state_revision","expected","set"});check_paused();guard_revision(p);auto before=registers();auto g=before["general"];
    auto values=p.at("set"), expected=p.at("expected");require(values.is_object()&&!values.empty()&&expected.is_object()&&!expected.empty(),"set and expected must be nonempty objects");
    for(auto it=values.begin();it!=values.end();++it){auto k=it.key();require(g.contains(k)&&k!="memptr"&&k!="q","unsupported register");unsigned max=(k=="i"||k=="r")?255:(k=="im"?2:(k=="iff1"||k=="iff2"?1:65535));number(it.value(),max,"register outside range");require(expected.contains(k),"expected must guard each changed register");}
    for(auto it=expected.begin();it!=expected.end();++it){require(g.contains(it.key()),"unknown expected register");require(number(it.value(),65535,"invalid expected register")==g.at(it.key()).get<unsigned>(),"register expectation mismatch");}
    for(auto it=values.begin();it!=values.end();++it)set_register(it.key(),it.value().get<unsigned>());revision++;return {{"before",before},{"after",registers()},{"state_revision",revision}};
  }
  if(method=="execution.pause"){fields(p,{});if(!paused)stop("pause");auto r=registers();r["paused"]=true;r["stop_reason"]=last_stop;return r;}
  if(method=="execution.continue"||method=="execution.go"||method=="execution.step"||method=="execution.run_until") {
    check_paused();bool step=method=="execution.step", until=method=="execution.run_until";
    if(step){fields(p,{"mode"});require(p.value("mode",J("into"))=="into","only step into supported");}
    else if(until){fields(p,{"predicate","max_emulated_ns"});validate_predicate(p.at("predicate"));}
    else fields(p,{});
    uint64_t budget=until?number(p.value("max_emulated_ns",J(1000000000ULL)),60000000000ULL,"invalid emulation budget"):0;
    require(!until||budget>0,"budget must be positive");
    if(step || last_stop.value("kind",std::string())=="breakpoint")exempt_pc=z80.pc.w;
    auto r=start(step);if(until){predicate=p.at("predicate");deadline=time_ns()+budget;}return r;
  }
  if(method=="execution.wait") {
    fields(p,{"operation_id","timeout_ms"});auto id=number(p.at("operation_id"),UINT64_MAX,"invalid operation ID");number(p.value("timeout_ms",J(0)),60000,"invalid timeout");
    if(id==active_op && id)return {{"operation_id",id},{"state","running"},{"running",true},{"paused",false}};
    for(auto &r:completed)if(r.at("operation_id")==id)return r;
    throw Invalid("unknown or expired operation ID");
  }
  if(method=="breakpoints.create") {
    fields(p,{"kind","address","once"});check_paused();require(p.value("kind",J("execution"))=="execution","only execution breakpoints supported");require(breakpoints.size()<256,"breakpoint limit");
    auto a=number(p.at("address"),65535,"invalid breakpoint address");require(!p.contains("once")||p.at("once").is_boolean(),"once must be boolean");
    J bp={{"breakpoint_id",next_bp++},{"kind","execution"},{"address",a},{"once",p.value("once",false)}};breakpoints.push_back(bp);revision++;bp["state_revision"]=revision;return bp;
  }
  if(method=="breakpoints.list"){fields(p,{});return {{"breakpoints",breakpoints}};}
  if(method=="breakpoints.delete"){fields(p,{"breakpoint_id"});check_paused();auto id=number(p.at("breakpoint_id"),UINT64_MAX,"invalid breakpoint ID");for(auto it=breakpoints.begin();it!=breakpoints.end();++it)if(it->at("breakpoint_id")==id){breakpoints.erase(it);revision++;return {{"deleted",id},{"state_revision",revision}};}throw Invalid("unknown breakpoint ID");}
  if(method=="input.keyboard") {
    fields(p,{"events"});check_paused();auto events=p.at("events");require(events.is_array()&&!events.empty()&&events.size()<=32,"need 1..32 key events");std::vector<std::pair<int,bool>> batch;
    for(auto&e:events){fields(e,{"key","pressed"});require(e.at("key").is_string()&&e.at("pressed").is_boolean(),"invalid key event");auto k=e.at("key").get<std::string>();int code=0;if(k=="enter")code=KEYBOARD_Enter;else if(k=="space")code=KEYBOARD_space;else if(k=="caps_shift")code=KEYBOARD_Caps;else if(k=="symbol_shift")code=KEYBOARD_Symbol;else if(k.size()==1&&((k[0]>='a'&&k[0]<='z')||(k[0]>='0'&&k[0]<='9')))code=k[0];require(code!=0,"unknown Spectrum key");batch.emplace_back(code,e.at("pressed").get<bool>());}
    for(auto&e:batch){if(e.second)keyboard_press(keyboard_key_name(e.first));else keyboard_release(keyboard_key_name(e.first));}revision++;return {{"accepted",batch.size()},{"state_revision",revision}};
  }
  throw std::out_of_range("method not found");
}
static std::string handle(const std::string &line) {
  J id=nullptr, message;bool notification=false;
  auto error=[&](int code,const std::string&why){return J{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",code},{"message",why}}}}.dump()+"\n";};
  try {message=J::parse(line);}catch(...){return error(-32700,"Parse error");}
  if(!message.is_object() || message.value("jsonrpc",J())!="2.0" || !message.contains("method") || !message["method"].is_string())return error(-32600,"Invalid request");
  if(message.contains("id")){id=message["id"];if(!(id.is_null()||id.is_string()||id.is_number_integer())){id=nullptr;return error(-32600,"Invalid ID");}}
  else notification=true;
  std::string result;
  try {auto value=dispatch(message["method"],message.value("params",J::object()));result=J{{"jsonrpc","2.0"},{"id",id},{"result",value}}.dump()+"\n";}
  catch(const Invalid&e){result=error(-32602,e.what());}
  catch(const J::exception&e){result=error(-32602,"Missing or invalid parameter");}
  catch(const std::out_of_range&e){result=error(-32601,e.what());}
  catch(const std::exception&e){result=error(-32603,e.what());}
  return notification?std::string():result;
}
static void client(SOCKET socket) {
  struct ReleaseSlot { ~ReleaseSlot(){client_count--;} } release_slot;
  DWORD timeout=60000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
  setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
  std::string buffer;char chunk[8192];
  for(;;) {
    int count=recv(socket,chunk,sizeof chunk,0);if(count<=0)break;buffer.append(chunk,count);
    for(;;){auto end=buffer.find('\n');if(end==std::string::npos)break;if(end>1048576){closesocket(socket);return;}
      auto request=std::make_shared<Request>();request->line=buffer.substr(0,end);buffer.erase(0,end+1);
      {std::lock_guard<std::mutex> lock(queue_mutex);if(requests.size()>=64){closesocket(socket);return;}requests.push_back(request);pending=true;}
      std::unique_lock<std::mutex> lock(request->mutex);request->cv.wait(lock,[&]{return request->done;});
      size_t offset=0;while(offset<request->response.size()){int n=send(socket,request->response.data()+offset,int(request->response.size()-offset),0);if(n<=0){closesocket(socket);return;}offset+=n;}
    }if(buffer.size()>1048576)break;
  }closesocket(socket);
}
extern "C" int pyro_rpc_arguments(int *argc,char **argv) {
  for(int i=1;i<*argc;i++)if(std::string(argv[i])=="--rpc-port") {
    if(i+1>=*argc){fprintf(stderr,"--rpc-port needs a number\n");return 1;}
    try {std::string s=argv[i+1];size_t consumed;port=std::stoi(s,&consumed);if(consumed!=s.size()||port<1||port>65535)throw std::runtime_error("port");}
    catch(...){fprintf(stderr,"invalid RPC port\n");return 1;}
    pyro_rpc_enabled=1;for(int j=i;j+2<*argc;j++)argv[j]=argv[j+2];*argc-=2;argv[*argc]=nullptr;i--;
  }return 0;
}
extern "C" int pyro_rpc_start() {
  if(!pyro_rpc_enabled)return 0;
  if(machine_current->machine!=LIBSPECTRUM_MACHINE_48){fprintf(stderr,"RPC currently requires --machine 48\n");return 1;}
  WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))return 1;
  auto s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  BOOL exclusive=TRUE;setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof exclusive);
  if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof address)||listen(s,16)){fprintf(stderr,"RPC bind failed: %d\n",WSAGetLastError());closesocket(s);return 1;}
  std::thread([s]{for(;;){auto c=accept(s,nullptr,nullptr);if(c==INVALID_SOCKET)break;
    if(client_count.fetch_add(1)>=16){client_count--;closesocket(c);continue;}
    try{std::thread(client,c).detach();}catch(...){client_count--;closesocket(c);}
  }}).detach();
  keyboard_release_all();last_stop["registers"]=registers();fprintf(stderr,"Fuse native RPC ready 127.0.0.1:%d (paused)\n",port);fflush(stderr);return 0;
}
extern "C" int pyro_rpc_boundary() {
  if(!pyro_rpc_enabled)return 0;
  if(pyro_rpc_trace_enabled&&trace_event_pending){trace_events+=clock_value()-trace_end_clock;trace_event_pending=false;}
  do {
    if(pending.load()) {
      for(int i=0;i<16;i++) {
        std::shared_ptr<Request> request;
        {std::lock_guard<std::mutex> lock(queue_mutex);if(requests.empty()){pending=false;break;}request=requests.front();requests.pop_front();}
        auto result=handle(request->line);
        {std::lock_guard<std::mutex> lock(request->mutex);request->response=std::move(result);request->done=true;}request->cv.notify_one();
      }
    }
    if(!paused) {
      if(pyro_rpc_trace_enabled&&(trace_units>=trace_max_units||trace_instructions.size()>=trace_max_records||trace_dropped_writes)) {
        pyro_rpc_trace_enabled=0;trace_end_clock=clock_value();stop("trace_limit",trace_status());
      }
      else if(step_target && instructions>=step_target)stop("step");
      else if(!predicate.is_null() && matches(predicate))stop("predicate",{{"predicate",predicate}});
      else if(deadline && time_ns()>=deadline)stop("timeout");
      else if(exempt_pc!=z80.pc.w) {
        for(size_t i=0;i<breakpoints.size();i++)if(breakpoints[i]["address"]==z80.pc.w){auto bp=breakpoints[i];if(bp["once"].get<bool>())breakpoints.erase(breakpoints.begin()+i);stop("breakpoint",{{"breakpoint_id",bp["breakpoint_id"]},{"address",z80.pc.w}});break;}
      }
    }
    if(paused) {
#ifndef UI_NULL
      ui_event();
#endif
      Sleep(2);
    }
  }while(paused && !fuse_exiting);
  exempt_pc=-1;if(!fuse_exiting)trace_begin_unit();return fuse_exiting;
}
extern "C" void pyro_rpc_advanced(){if(pyro_rpc_enabled){instructions++;revision++;
  if(pyro_rpc_trace_enabled&&trace_inflight){trace_current.end_clock=clock_value();trace_current.next_pc=z80.pc.w;
    trace_pc_counts[trace_current.pc]++;trace_pc_cycles[trace_current.pc]+=trace_current.end_clock-trace_current.clock;
    trace_pc_waits[trace_current.pc]+=trace_unit_waits;trace_unit_waits=0;trace_units++;
    if(!trace_current.opcode.empty())trace_instructions.push_back(std::move(trace_current));
    trace_end_clock=clock_value();trace_inflight=false;trace_event_pending=true;}
}}
extern "C" void pyro_rpc_record_contention(unsigned cycles) {
  if (trace_inflight) trace_unit_waits += cycles;
}
extern "C" void pyro_rpc_audio_ula(unsigned at_tstates,int mic_on,int beeper_on) {
  bool changed=audio_mic!=!!mic_on||audio_ear!=!!beeper_on;
  audio_mic=!!mic_on;audio_ear=!!beeper_on;
  if(!pyro_rpc_audio_enabled||!changed)return;
  if(audio_edges.size()>=200000){audio_dropped++;return;}
  audio_edges.push_back({uint64_t(spectrum_get_frame_count())*machine_current->timings.tstates_per_frame+at_tstates,audio_mic,audio_ear});
}
extern "C" void pyro_rpc_audio_pcm(const short *data,unsigned count,unsigned channels) {
  if(!pyro_rpc_audio_enabled)return;
  if(channels!=audio_channels||audio_pcm.size()+count>audio_max_frames*channels){audio_dropped+=count;return;}
  audio_pcm.insert(audio_pcm.end(),data,data+count);
}
