// One-shot debugger for an exact instruction in our own addon. No target calls.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kAddon[] = L"C:\\XboxGames\\Microsoft Flight Simulator 2024\\Content\\taxi-camera-native.addon64";
constexpr wchar_t kFixture[] = L"C:\\Repos\\aircraft\\tools\\taxi-camera-native\\discovery\\build\\debug-hw-fixture.exe";
constexpr char kHash[] = "BFD688FE87E363DE3BFFB69B132EB29C93CABD55D5974EF333B45C6C425E2EC9";
constexpr char kPfdHash[] = "F991D7681B1A5F1A565F1190DD89DD39FE74FDA5AE5974ED8662F2DCFF43DF97";
constexpr std::array<unsigned char,7> kInstruction{0x48,0xff,0x86,0xd8,0x25,0x46,0x02};
struct Handle { HANDLE value=nullptr; ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);} };
struct Module { std::uint64_t base=0,size=0; std::wstring path; };
struct Near { std::uint64_t pointer=0,vtable=0,generation=0; long long delta=0; unsigned slot=0; };
struct Report {
  const char* error="not_started"; DWORD win_error=0,pid=0,thread=0,detach_error=0; bool fixture=false,parallel=false,comparison=false,pfd=false,hit=false,restored=false,detached=false,rewound=false,debugger_check=false,debugger_present=true;
  std::uint64_t breakpoint=0,rip=0,rsp=0,rsi=0,rdi=0,rbx=0,r10=0,list=0,vtable=0,queue=0;
  unsigned registered=0,exact=0,events=0,bytes=0,drained=0,extra_breakpoints=0,detach_attempts=0,same_vtables=0,changed=0; std::vector<Near> nearby,known_samples; std::vector<Module> modules;
  std::uint64_t selected_id=0,target_id=0,binding_id=0,object_generation=0,view=0;
  std::uint32_t width=0,height=0,format=0;
  std::array<unsigned char,7> command_flags{};
  unsigned char camera_enabled=0,device_lost=0;
};
const wchar_t* basename(const wchar_t* path){const auto* slash=wcsrchr(path,L'\\');return slash?slash+1:path;}
bool read(HANDLE process,std::uint64_t address,void* destination,std::size_t count,Report& report){
  if(!address||!count||count>32768||address>UINT64_MAX-count||report.bytes>262144-count)return false;
  report.bytes+=static_cast<unsigned>(count); SIZE_T copied=0;
  return ReadProcessMemory(process,reinterpret_cast<const void*>(address),destination,count,&copied)&&copied==count;
}
bool image_modules(HANDLE process,Report& report){
  std::array<HMODULE,512> modules{};DWORD needed=0;
  if(!K32EnumProcessModulesEx(process,modules.data(),sizeof(modules),&needed,LIST_MODULES_64BIT)||!needed||needed>sizeof(modules)||needed%8)return false;
  for(unsigned i=0;i<needed/8;++i){
    std::array<wchar_t,32768> path{};MODULEINFO info{};
    if(!K32GetModuleFileNameExW(process,modules[i],path.data(),path.size())||!K32GetModuleInformation(process,modules[i],&info,sizeof(info)))return false;
    report.modules.push_back({reinterpret_cast<std::uint64_t>(info.lpBaseOfDll),info.SizeOfImage,path.data()});
  }
  return true;
}
bool file_match(const wchar_t* loaded,const char* expected_hash=kHash){
  Handle expected,actual; expected.value=CreateFileW(kAddon,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  actual.value=CreateFileW(loaded,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  BY_HANDLE_FILE_INFORMATION a{},b{};LARGE_INTEGER size{};
  if(expected.value==INVALID_HANDLE_VALUE||actual.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(expected.value,&a)||
     !GetFileInformationByHandle(actual.value,&b)||a.dwVolumeSerialNumber!=b.dwVolumeSerialNumber||a.nFileIndexHigh!=b.nFileIndexHigh||
     a.nFileIndexLow!=b.nFileIndexLow||!GetFileSizeEx(expected.value,&size)||size.QuadPart<1||size.QuadPart>2*1024*1024)return false;
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));DWORD copied=0;
  if(!ReadFile(expected.value,bytes.data(),bytes.size(),&copied,nullptr)||copied!=bytes.size())return false;
  BCRYPT_ALG_HANDLE algorithm=nullptr;if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
  std::array<unsigned char,32> digest{};BCRYPT_HASH_HANDLE hash=nullptr;
  auto hashed=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
  if(hashed>=0)hashed=BCryptHashData(hash,bytes.data(),bytes.size(),0);
  if(hashed>=0)hashed=BCryptFinishHash(hash,digest.data(),digest.size(),0);
  if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);if(hashed<0)return false;
  char hex[65]{};for(unsigned i=0;i<32;++i)std::snprintf(hex+i*2,3,"%02X",digest[i]);return std::strcmp(hex,expected_hash)==0;
}
bool context(HANDLE thread,DWORD expected,CONTEXT& value){
  value={};value.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
  return GetProcessIdOfThread(thread)==expected&&GetThreadContext(thread,&value);
}
bool rewind(HANDLE thread,DWORD expected,std::uint64_t address){
  for(unsigned attempt=0;attempt<8;++attempt){
    alignas(16) CONTEXT value{};
    if(context(thread,expected,value)&&(value.Rip==address||value.Rip==address+1)){
      value.Rip=address;
      if(SetThreadContext(thread,&value)){
        alignas(16) CONTEXT checked{};
        if(context(thread,expected,checked)&&checked.Rip==address)return true;
      }
    }
    Sleep(5);
  }
  return false;
}
struct Patch {
  HANDLE process=nullptr;std::uint64_t address=0;unsigned char original=0;DWORD protection=0;bool armed=false,have_protection=false;
  bool put(unsigned char value){
    DWORD old=0;
    if(!VirtualProtectEx(process,reinterpret_cast<void*>(address),1,PAGE_EXECUTE_READWRITE|PAGE_TARGETS_NO_UPDATE,&old))return false;
    if(!have_protection){protection=old;have_protection=true;}
    SIZE_T copied=0;const BOOL wrote=WriteProcessMemory(process,reinterpret_cast<void*>(address),&value,1,&copied);
    if(wrote&&copied==1)armed=value!=original;
    const BOOL flushed=FlushInstructionCache(process,reinterpret_cast<void*>(address),1);
    DWORD ignored=0;const BOOL restored=VirtualProtectEx(process,reinterpret_cast<void*>(address),1,protection,&ignored);
    unsigned char check=0;SIZE_T read_bytes=0;
    return wrote&&copied==1&&flushed&&restored&&ReadProcessMemory(process,reinterpret_cast<void*>(address),&check,1,&read_bytes)&&read_bytes==1&&check==value;
  }
  bool restore(){for(unsigned i=0;i<8;++i){if(put(original))return true;Sleep(5);}return false;}
};
bool capture(HANDLE process,const CONTEXT& state,Report& report){
  report.rip=state.Rip;report.rsp=state.Rsp;report.rsi=state.Rsi;report.rdi=state.Rdi;report.rbx=state.Rbx;report.r10=state.R10;
  if(report.fixture)return true;
  if(report.pfd){
    // These are exact 0.7.6 native guard registers/offsets, independently checked
    // against its installed instructions. No engine object or target call.
    if(!state.R13||!state.R14||!state.R15)return false;
    if(!read(process,state.R14+0xa0,&report.selected_id,8,report)||
       !read(process,state.R15+0x48,&report.target_id,8,report)||
       !read(process,state.R13+0x18,&report.binding_id,8,report)||
       !read(process,state.R13,&report.view,8,report)||
       !read(process,state.R13+0x50,&report.object_generation,8,report)||
       !read(process,state.R13+0x48,report.command_flags.data(),report.command_flags.size(),report)||
       !read(process,state.R13+0x40,&report.width,4,report)||
       !read(process,state.R13+0x44,&report.height,4,report)||
       !read(process,state.R13+0x2c,&report.format,4,report)||
       !read(process,state.R14+0x14a,&report.camera_enabled,1,report)||
       !read(process,state.R14+0x14b,&report.device_lost,1,report))return false;
    return report.selected_id==10943&&report.target_id==report.selected_id&&report.binding_id==report.target_id;
  }
  if(!state.Rsi||!state.R10||state.R10>256||state.Rbx>=state.R10)return false;
  std::uint64_t array=0;
  if(!read(process,state.Rsp+0x58,&array,8,report)||!array||!read(process,array+state.Rbx*8,&report.list,8,report)||
     !read(process,report.list,&report.vtable,8,report)||!read(process,state.Rsp+0x60,&report.queue,8,report))return false;
  for(unsigned i=0;i<4096;++i){
    const auto entry=state.Rsi+12472+static_cast<std::uint64_t>(i)*9296;std::uint64_t pointer=0;
    if(!read(process,entry,&pointer,8,report))return false;
    if(!pointer)continue;++report.registered;report.exact+=pointer==report.list;
    if(report.known_samples.size()<8){
      Near sample;sample.pointer=pointer;sample.slot=i;
      if(read(process,pointer,&sample.vtable,8,report))report.known_samples.push_back(sample);
    }
    const auto distance=pointer>report.list?pointer-report.list:report.list-pointer;
    if(distance<=4096){
      Near value;value.pointer=pointer;value.delta=static_cast<long long>(pointer-report.list);value.slot=i;
      if(!read(process,entry+16,&value.generation,8,report))return false;
      (void)read(process,pointer,&value.vtable,8,report);report.nearby.push_back(value);
    }
  }
  return true;
}
void close_event_handles(DEBUG_EVENT& event){
  // Windows owns CREATE_PROCESS/CREATE_THREAD process/thread handles and closes
  // them on the corresponding exit event. Only image-file handles are ours.
  if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT){
    if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
  }else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT){if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);}
}
bool drain(HANDLE process,Patch& patch,Report& report){
  // Other CPUs can have trapped before the first debug event suspended them.
  // The byte is already restored, but each queued exact breakpoint still needs
  // its own RIP rewind. Unrelated application exceptions remain unhandled.
  const auto deadline=GetTickCount64()+100;
  for(unsigned i=0;i<64&&GetTickCount64()<deadline;++i){
    DEBUG_EVENT event{};
    if(!WaitForDebugEvent(&event,5))return GetLastError()==ERROR_SEM_TIMEOUT;
    ++report.drained;DWORD status=DBG_CONTINUE;bool okay=true;
    if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT){
      const auto& exception=event.u.Exception.ExceptionRecord;
      if(exception.ExceptionCode==EXCEPTION_BREAKPOINT&&reinterpret_cast<std::uint64_t>(exception.ExceptionAddress)==patch.address){
        ++report.extra_breakpoints;Handle thread;
        thread.value=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,event.dwThreadId);
        okay=patch.restore()&&thread.value&&rewind(thread.value,report.pid,patch.address);
      }else status=DBG_EXCEPTION_NOT_HANDLED;
    }
    close_event_handles(event);
    if(!okay){report.error="pending_breakpoint_restore";return false;}
    if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,status)){report.error="pending_continue";return false;}
    if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT)return true;
  }
  (void)process;
  return true;
}
bool run(Report& report){
  // No dynamic allocation while an armed breakpoint or stopped target is held.
  report.nearby.reserve(4096);
  report.known_samples.reserve(8);
  Handle process;process.value=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,report.pid);
  if(!process.value){report.error="open_process";return false;}
  std::array<wchar_t,32768> executable{};DWORD length=executable.size();
  if(!QueryFullProcessImageNameW(process.value,0,executable.data(),&length)||_wcsicmp(basename(executable.data()),report.fixture?L"debug-hw-fixture.exe":L"FlightSimulator2024.exe")){
    report.error="process_identity";return false;
  }
  if(!image_modules(process.value,report)){report.error="module_inventory";return false;}
  unsigned matches=0;
  for(const auto& module:report.modules){
    if(_wcsicmp(basename(module.path.c_str()),report.fixture?L"debug-hw-fixture.exe":L"taxi-camera-native.addon64"))continue;
    if(!report.fixture&&!file_match(module.path.c_str(),report.pfd?kPfdHash:kHash)){report.error="addon_file_identity_or_hash";return false;}
    report.breakpoint=module.base+(report.fixture?0x1465:report.pfd?0x5bb4:0x17530);++matches;
  }
  if(matches!=1){report.error="module_identity";return false;}
  std::array<unsigned char,7> bytes{};const std::array<unsigned char,7> fixture_bytes{0x8b,0x44,0x24,0x04,0x83,0xc0,0x01};
  const std::array<unsigned char,7> pfd_instruction{0x41,0x80,0x7d,0x4e,0x01,0x75,0x12};
  const auto expected=report.fixture?fixture_bytes:report.pfd?pfd_instruction:kInstruction;
  if(!read(process.value,report.breakpoint,bytes.data(),bytes.size(),report)||bytes!=expected){report.error="instruction_guard";return false;}
  if(!DebugActiveProcess(report.pid)){report.error="debug_attach";return false;}
  if(!DebugSetProcessKillOnExit(FALSE)){report.error="disable_kill";report.detached=DebugActiveProcessStop(report.pid);return false;}
  Patch patch{process.value,report.breakpoint,expected[0]};bool installed=false,exit_seen=false;
  const auto deadline=GetTickCount64()+10000;report.error="breakpoint_timeout";
  while(GetTickCount64()<deadline){
    DEBUG_EVENT event{};
    if(!WaitForDebugEvent(&event,100)){if(GetLastError()==ERROR_SEM_TIMEOUT)continue;report.error="wait_event";break;}
    ++report.events;DWORD continue_status=DBG_CONTINUE;bool done=false;
    if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT){exit_seen=true;report.error="process_exited";done=true;}
    if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT){
      const auto& exception=event.u.Exception.ExceptionRecord;
      const auto exception_address=reinterpret_cast<std::uint64_t>(exception.ExceptionAddress);
      if(exception.ExceptionCode==EXCEPTION_BREAKPOINT&&installed&&exception_address==report.breakpoint){
        report.thread=event.dwThreadId;Handle thread;
        thread.value=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,event.dwThreadId);
        alignas(16) CONTEXT current{};
        bool have_context=false;
        for(unsigned attempt=0;thread.value&&attempt<8&&!have_context;++attempt){have_context=context(thread.value,report.pid,current);if(!have_context)Sleep(5);}
        if(!have_context)report.error="breakpoint_context";
        else if(current.Rip!=report.breakpoint&&current.Rip!=report.breakpoint+1)report.error="breakpoint_rip";
        else{
          report.hit=true;const bool captured=capture(process.value,current,report);
          report.restored=patch.restore();
          if(report.restored)report.rewound=rewind(thread.value,report.pid,report.breakpoint);
          report.error=!report.restored?"restore_failed":!report.rewound?"rewind_failed":!captured?"capture_failed":"none";
        }
        if(!report.restored)report.restored=patch.restore();
        if(!report.rewound&&thread.value)report.rewound=rewind(thread.value,report.pid,report.breakpoint);
        done=true;
      }else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT&&!installed){
        // Windows' initial debugger attachment breakpoint. It is not recorded.
        if(!read(process.value,report.breakpoint,bytes.data(),bytes.size(),report)||bytes!=expected){report.error="attached_instruction_guard";done=true;}
        else if(!(installed=patch.put(0xcc))){report.error="install_breakpoint";done=true;}
      }else continue_status=DBG_EXCEPTION_NOT_HANDLED;
    }
    // Restore before releasing any debug event on an installation/error path.
    if(done&&patch.armed)report.restored=patch.restore();
    close_event_handles(event);
    if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continue_status)){report.error="continue_event";break;}
    if(done)break;
  }
  report.win_error=std::strcmp(report.error,"none")==0?0:GetLastError();
  if(!exit_seen){
    if(patch.have_protection)report.restored=patch.restore();
    else report.restored=true;
    for(unsigned attempt=0;attempt<8&&!report.detached;++attempt){
      if(!drain(process.value,patch,report))break;
      ++report.detach_attempts;
      if(DebugActiveProcessStop(report.pid))report.detached=true;
      else report.detach_error=GetLastError();
      BOOL present=TRUE;
      if(CheckRemoteDebuggerPresent(process.value,&present)){
        report.debugger_check=true;report.debugger_present=present!=FALSE;
        if(!present)report.detached=true;
      }
      if(!report.detached)Sleep(10);
    }
    if(!report.detached){report.error="debug_detach";report.win_error=report.detach_error;}
    if(!read(process.value,report.breakpoint,bytes.data(),bytes.size(),report)||bytes!=expected)report.restored=false;
  }
  return report.hit&&report.restored&&report.rewound&&report.detached&&std::strcmp(report.error,"none")==0;
}
bool compare(Report& report){
  // This capture's absolute addresses are valid only for its still-running PID.
  // The exact installed DLL and unchanged live singleton are checked first.
  constexpr std::uint64_t manager=1956575895616,unknown_list=1956703164736,unknown_vtable=140733040617520;
  if(report.pid!=20204){report.error="wrong_capture_pid";return false;}
  Handle process;process.value=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,report.pid);
  if(!process.value){report.error="open_readonly_process";return false;}
  std::array<wchar_t,32768> executable{};DWORD length=executable.size();
  if(!QueryFullProcessImageNameW(process.value,0,executable.data(),&length)||_wcsicmp(basename(executable.data()),L"FlightSimulator2024.exe")){
    report.error="process_identity";return false;
  }
  if(!image_modules(process.value,report)){report.error="module_inventory";return false;}
  std::uint64_t addon=0;bool core=false;
  for(const auto& module:report.modules){
    if(!_wcsicmp(basename(module.path.c_str()),L"taxi-camera-native.addon64")){
      if(addon||!file_match(module.path.c_str())){report.error="addon_identity_or_hash";return false;}addon=module.base;
    }
    if(!_wcsicmp(basename(module.path.c_str()),L"D3D12Core.dll")&&module.base==unknown_vtable-3205168)core=true;
  }
  std::uint64_t current=0;
  if(!addon||!core||!read(process.value,addon+0x9e778,&current,8,report)||current!=manager){report.error="saved_capture_identity_changed";return false;}
  report.rsi=manager;report.list=unknown_list;report.vtable=unknown_vtable;
  for(unsigned i=0;i<4096;++i){
    const auto entry=manager+12472+static_cast<std::uint64_t>(i)*9296;std::uint64_t native=0,vtable=0,again=0;
    if(!read(process.value,entry,&native,8,report)){report.error="list_read";return false;}
    if(!native)continue;
    if(!read(process.value,native,&vtable,8,report)||!read(process.value,entry,&again,8,report)||again!=native){++report.changed;continue;}
    ++report.registered;report.exact+=native==unknown_list;report.same_vtables+=vtable==unknown_vtable;
    if(report.known_samples.size()<8){Near sample;sample.slot=i;sample.pointer=native;sample.vtable=vtable;report.known_samples.push_back(sample);}
  }
  if(!read(process.value,addon+0x9e778,&current,8,report)||current!=manager){report.error="manager_changed";return false;}
  BOOL present=TRUE;report.debugger_check=CheckRemoteDebuggerPresent(process.value,&present)!=FALSE;report.debugger_present=present!=FALSE;
  report.error="none";return true;
}
void print_location(const Report& report,std::uint64_t address){
  for(const auto& module:report.modules)if(address>=module.base&&address-module.base<module.size){
    char name[512]{};WideCharToMultiByte(CP_UTF8,0,basename(module.path.c_str()),-1,name,sizeof(name),nullptr,nullptr);
    std::printf("{\"module\":\"%s\",\"rva\":%llu}",name,address-module.base);return;
  }
  std::printf("{\"module\":\"outside_module_images\"}");
}
} // namespace
int main(int argc,char** argv){
  Report report;
  if(argc==2&&(std::strcmp(argv[1],"--fixture")==0||std::strcmp(argv[1],"--fixture-parallel")==0)){
    report.fixture=true;report.parallel=std::strcmp(argv[1],"--fixture-parallel")==0;STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
    wchar_t command[32768]{};std::swprintf(command,std::size(command),L"\"%s\"%s",kFixture,report.parallel?L" --parallel":L"");
    if(!CreateProcessW(kFixture,command,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))return 3;
    report.pid=info.dwProcessId;CloseHandle(info.hThread);CloseHandle(info.hProcess);Sleep(100);
  }else if(argc==3&&(std::strcmp(argv[1],"--pid")==0||std::strcmp(argv[1],"--compare-pid")==0||std::strcmp(argv[1],"--pfd-pid")==0)){
    report.comparison=std::strcmp(argv[1],"--compare-pid")==0;
    report.pfd=std::strcmp(argv[1],"--pfd-pid")==0;
    auto parsed=std::from_chars(argv[2],argv[2]+std::strlen(argv[2]),report.pid);if(parsed.ec!=std::errc{}||!report.pid)return 2;
  }else return 2;
  const bool passed=report.comparison?compare(report):run(report);
  std::printf("{\"passed\":%s,\"fixture\":%s,\"pid\":%lu,\"error\":\"%s\",\"windows_error\":%lu,\"hit\":%s,\"restored\":%s,\"rewound\":%s,\"detached\":%s,\"events\":%u,\"bytes\":%u,\"thread_id\":%lu,\"breakpoint\":%llu,\"rip\":%llu,\"rsp\":%llu,\"rsi\":%llu,\"rdi\":%llu,\"index\":%llu,\"count\":%llu,\"list\":%llu,\"queue\":%llu,\"vtable\":%llu,\"vtable_location\":",
    passed?"true":"false",report.fixture?"true":"false",report.pid,report.error,report.win_error,report.hit?"true":"false",report.restored?"true":"false",report.rewound?"true":"false",report.detached?"true":"false",report.events,report.bytes,report.thread,report.breakpoint,report.rip,report.rsp,report.rsi,report.rdi,report.rbx,report.r10,report.list,report.queue,report.vtable);
  print_location(report,report.vtable);
  std::printf(",\"registered_count\":%u,\"exact_registered_count\":%u,\"nearby_registered\":[",report.registered,report.exact);
  for(std::size_t i=0;i<report.nearby.size();++i){const auto& entry=report.nearby[i];if(i)std::printf(",");
    std::printf("{\"slot\":%u,\"list\":%llu,\"delta\":%lld,\"generation\":%llu,\"vtable\":%llu,\"vtable_location\":",entry.slot,entry.pointer,entry.delta,entry.generation,entry.vtable);print_location(report,entry.vtable);std::printf("}");}
  std::printf("],\"known_registered_samples\":[");
  for(std::size_t i=0;i<report.known_samples.size();++i){const auto& entry=report.known_samples[i];if(i)std::printf(",");
    std::printf("{\"slot\":%u,\"list\":%llu,\"vtable\":%llu,\"vtable_location\":",entry.slot,entry.pointer,entry.vtable);print_location(report,entry.vtable);std::printf("}");}
  std::printf("],\"drained_events\":%u,\"extra_breakpoints\":%u,\"detach_attempts\":%u,\"detach_error\":%lu,\"debugger_check\":%s,\"debugger_present\":%s,\"readonly_comparison\":%s,\"same_vtable_count\":%u,\"changed_entries\":%u",report.drained,report.extra_breakpoints,report.detach_attempts,report.detach_error,report.debugger_check?"true":"false",report.debugger_present?"true":"false",report.comparison?"true":"false",report.same_vtables,report.changed);
  std::printf(",\"pfd_guard\":%s,\"selected_id\":%llu,\"target_id\":%llu,\"binding_id\":%llu,\"object_generation\":%llu,\"width\":%u,\"height\":%u,\"format\":%u,\"direct\":%u,\"inside_native_pass\":%u,\"unsupported_recording\":%u,\"has_depth_stencil\":%u,\"capture_ready\":%u,\"camera_enabled\":%u,\"device_lost\":%u}\n",report.pfd?"true":"false",report.selected_id,report.target_id,report.binding_id,report.object_generation,report.width,report.height,report.format,report.command_flags[0],report.command_flags[2],report.command_flags[3],report.command_flags[5],report.command_flags[6],report.camera_enabled,report.device_lost);
  return passed?0:1;
}
