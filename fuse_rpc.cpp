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
}
using J = nlohmann::json;
int pyro_rpc_enabled = 0;
static int port = 2304;
static bool paused = true;
static uint64_t revision = 1, next_op = 1, next_bp = 1, active_op = 0;
static uint64_t instructions = 0, step_target = 0, deadline = 0;
static int exempt_pc = -1;
static J predicate = nullptr, breakpoints = J::array(), completed = J::array();
static J last_stop = {{"kind", "startup"}};
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
    {"methods",{"agent.capabilities","emulator.info","session.status","state.get","state.get_registers","state.observe","state.set_registers","memory.read","memory.write","execution.pause","execution.continue","execution.go","execution.step","execution.run_until","execution.wait","breakpoints.create","breakpoints.list","breakpoints.delete","input.keyboard","video.snapshot"}},
    {"limits",{{"address_bits",16},{"max_line_bytes",1048576},{"max_queue",64},{"max_breakpoints",256},{"max_memory_bytes",65536}}},
    {"unsupported",{"watchpoints","conditional_breakpoints","step_over","reverse_execution","complete_machine_snapshots","GUI","XT_scan_codes"}}};
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
  if(method=="agent.capabilities" || method=="emulator.info"){fields(p,{});return capabilities();}
  if(method=="session.status") {
    fields(p,{"session_id"});if(p.contains("session_id"))require(p.at("session_id")=="fuse","unknown session");
    return {{"session_id","fuse"},{"state",paused?"paused":"running"},{"paused",paused},{"state_revision",revision},{"clock",clock_value()},{"emulated_time_ns",time_ns()},{"last_stop",last_stop}};
  }
  if(method=="state.get" || method=="state.get_registers"){fields(p,{});return registers();}
  if(method=="memory.read"){fields(p,{"address","length"});return descriptor(number(p.at("address"),65535,"invalid address"),number(p.value("length",J(1)),65536,"invalid length"));}
  if(method=="video.snapshot"){fields(p,{});auto v=descriptor(16384,6912);v["adapter"]="ZX Spectrum ULA";v["width"]=256;v["height"]=192;v["format"]="spectrum-screen";return v;}
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
      if(step_target && instructions>=step_target)stop("step");
      else if(!predicate.is_null() && matches(predicate))stop("predicate",{{"predicate",predicate}});
      else if(deadline && time_ns()>=deadline)stop("timeout");
      else if(exempt_pc!=z80.pc.w) {
        for(size_t i=0;i<breakpoints.size();i++)if(breakpoints[i]["address"]==z80.pc.w){auto bp=breakpoints[i];if(bp["once"].get<bool>())breakpoints.erase(breakpoints.begin()+i);stop("breakpoint",{{"breakpoint_id",bp["breakpoint_id"]},{"address",z80.pc.w}});break;}
      }
    }
    if(paused)Sleep(2);
  }while(paused && !fuse_exiting);
  exempt_pc=-1;return fuse_exiting;
}
extern "C" void pyro_rpc_advanced(){if(pyro_rpc_enabled){instructions++;revision++;
}}
