"""Bounded authorized breakpoint test. No target expressions or method calls."""
import ctypes, hashlib, json, os, struct, time, threading
import lldb

class NativeRegisters(ctypes.Structure):
    _fields_ = [(name,ctypes.c_uint64) for name in ('rip','rsp','rsi','rdi','rbx','r10')] + [('error',ctypes.c_uint32),('flags',ctypes.c_uint32)]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K = ctypes.WinDLL('kernel32', use_last_error=True)
K.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
K.OpenProcess.restype = ctypes.c_void_p
K.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
K.ReadProcessMemory.restype = ctypes.c_int
K.CloseHandle.argtypes = [ctypes.c_void_p]
K.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p,ctypes.c_ulong,ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_ulong)]
K.K32EnumProcessModulesEx.argtypes = [ctypes.c_void_p,ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.c_ulong]
K.K32GetModuleFileNameExW.argtypes = [ctypes.c_void_p,ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_ulong]

def modules(handle):
    array = (ctypes.c_void_p * 512)()
    needed = ctypes.c_ulong()
    if not K.K32EnumProcessModulesEx(handle,array,ctypes.sizeof(array),ctypes.byref(needed),2) or needed.value > ctypes.sizeof(array):
        raise RuntimeError('module bounds or read failure')
    result=[]
    for base in list(array)[:needed.value//8]:
        path=ctypes.create_unicode_buffer(32768)
        if not K.K32GetModuleFileNameExW(handle,base,path,len(path)): raise RuntimeError('module name failed')
        dos=read(handle,base,64)
        if dos[:2]!=b'MZ': raise RuntimeError('module DOS header')
        pe=struct.unpack_from('<I',dos,60)[0]
        if pe>1048576: raise RuntimeError('module PE bound')
        header=read(handle,base+pe,84)
        if header[:4]!=b'PE\0\0': raise RuntimeError('module PE header')
        size=struct.unpack_from('<I',header,80)[0]
        result.append({'base':base,'size':size,'path':path.value})
    return result

def location(items,address):
    for item in items:
        if item['base']<=address<item['base']+item['size']:
            return {'module':os.path.basename(item['path']),'rva':hex(address-item['base'])}
    return {'module':'outside_module_images'}

def restore_boundary(helper,pid,tid,address):
    context=NativeRegisters()
    if not helper.read_debug_thread(pid,tid,ctypes.byref(context)):
        raise RuntimeError('read context before detach failed')
    if context.rip==address+1:
        if not helper.rewind_debug_thread(pid,tid,address+1,address):
            raise RuntimeError('exact breakpoint RIP rewind failed')
        if not helper.read_debug_thread(pid,tid,ctypes.byref(context)):
            raise RuntimeError('read rewound context failed')
    if context.rip!=address:
        raise RuntimeError('instruction boundary changed before detach')
    return hex(context.rip)

def live(debugger,pid):
    report={'mode':'live','pid':pid,'passed':False}
    process=None; handle=None; bp=None; address=None; hit_tid=None; helper=None
    expected=bytes.fromhex('48ff86d8254602')
    try:
        if not json.load(open(os.path.join(ROOT,'discovery','build','debug-software-fixture.json')))['passed']:
            raise RuntimeError('local fixture prerequisite failed')
        if not json.load(open(os.path.join(ROOT,'discovery','build','debug-native-context-fixture.json')))['passed']:
            raise RuntimeError('local native context fixture prerequisite failed')
        helper=ctypes.WinDLL(os.path.join(ROOT,'discovery','build','debug-native-context.dll'))
        helper.read_debug_thread.argtypes=[ctypes.c_ulong,ctypes.c_ulong,ctypes.POINTER(NativeRegisters)]
        helper.read_debug_thread.restype=ctypes.c_int
        helper.rewind_debug_thread.argtypes=[ctypes.c_ulong,ctypes.c_ulong,ctypes.c_uint64,ctypes.c_uint64]
        helper.rewind_debug_thread.restype=ctypes.c_int
        handle=K.OpenProcess(0x410,False,pid)
        if not handle: raise RuntimeError('process open failed %d'%ctypes.get_last_error())
        path=ctypes.create_unicode_buffer(32768); length=ctypes.c_ulong(len(path))
        if not K.QueryFullProcessImageNameW(handle,0,path,ctypes.byref(length)) or os.path.basename(path.value).lower()!='flightsimulator2024.exe':
            raise RuntimeError('wrong process')
        items=modules(handle)
        hits=[item for item in items if os.path.basename(item['path']).lower()=='taxi-camera-native.addon64']
        installed=r'C:\XboxGames\Microsoft Flight Simulator 2024\Content\taxi-camera-native.addon64'
        if len(hits)!=1 or not os.path.samefile(hits[0]['path'],installed): raise RuntimeError('loaded DLL identity')
        with open(installed,'rb') as source: digest=hashlib.sha256(source.read(2*1024*1024)).hexdigest().upper()
        if digest!='BFD688FE87E363DE3BFFB69B132EB29C93CABD55D5974EF333B45C6C425E2EC9': raise RuntimeError('installed DLL hash')
        address=hits[0]['base']+0x17530
        if read(handle,address,len(expected))!=expected: raise RuntimeError('exact instruction guard')
        report['guarded_sha256']=digest
        debugger.HandleCommand('settings set target.detach-on-error true')
        debugger.SetAsync(False)
        target=debugger.CreateTargetWithFileAndArch(installed,'x86_64')
        error=lldb.SBError()
        process=target.AttachToProcessWithID(debugger.GetListener(),pid,error)
        if error.Fail() or process.GetState()!=lldb.eStateStopped: raise RuntimeError('attach failed '+str(error))
        if read(handle,address,len(expected))!=expected: raise RuntimeError('post-attach instruction guard')
        bp=target.BreakpointCreateByAddress(address)
        bp.SetOneShot(True)
        if bp.GetNumLocations()!=1: raise RuntimeError('breakpoint location refused')
        debugger.SetAsync(False)
        prior_stop=process.GetStopID()
        watchdog=threading.Timer(10,process.Stop)
        watchdog.daemon=True
        watchdog.start()
        try:
            error=process.Continue()
        finally:
            watchdog.cancel()
        if error.Fail(): raise RuntimeError('continue failed '+str(error))
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            state=process.GetState()
            if state in (lldb.eStateExited,lldb.eStateDetached) or (state==lldb.eStateStopped and process.GetStopID()>prior_stop):
                break
            time.sleep(.02)
        if process.GetState()!=lldb.eStateStopped: raise RuntimeError('one-shot timeout or process exit')
        if process.GetNumThreads()>512: raise RuntimeError('thread count bound')
        native_hits=[]
        report['stops']=[]
        for i in range(process.GetNumThreads()):
            candidate_thread=process.GetThreadAtIndex(i)
            context=NativeRegisters()
            tid=candidate_thread.GetThreadID()
            ok=helper.read_debug_thread(pid,tid,ctypes.byref(context))
            if candidate_thread.GetStopReason() not in (lldb.eStopReasonNone,lldb.eStopReasonInvalid):
                report['stops'].append({'id':tid,'reason':candidate_thread.GetStopReason(),'description':candidate_thread.GetStopDescription(256),'lldb_pc':hex(candidate_thread.GetFrameAtIndex(0).GetPC()),'native_pc':hex(context.rip),'native_error':context.error})
            if ok and context.rip in (address,address+1): native_hits.append((candidate_thread,context))
        if len(native_hits)!=1: raise RuntimeError('exact native breakpoint context count %d'%len(native_hits))
        thread,context=native_hits[0]
        hit_tid=thread.GetThreadID()
        report['native_thread']=hit_tid
        report['native_rip']=hex(context.rip)
        target.BreakpointDelete(bp.GetID())
        if read(handle,address,len(expected))!=expected: raise RuntimeError('breakpoint byte restoration failed')
        report['restored_rip']=restore_boundary(helper,pid,hit_tid,address)
        regs={name:getattr(context,name) for name in ('rsi','rbx','r10','rsp','rdi')}
        manager=regs['rsi']; index=regs['rbx']; count=regs['r10']
        if not manager or index>=count or count>256: raise RuntimeError('branch register bounds')
        array=struct.unpack('<Q',read(handle,regs['rsp']+0x58,8))[0]
        native=struct.unpack('<Q',read(handle,array+index*8,8))[0]
        vtable=struct.unpack('<Q',read(handle,native,8))[0]
        queue=struct.unpack('<Q',read(handle,regs['rsp']+0x60,8))[0]
        report.update({'list':hex(native),'vtable':hex(vtable),'vtable_location':location(items,vtable),'queue':hex(queue),'batch_count':count,'index':index,'rdi_matches_list':regs['rdi']==native})
        report['vtable_functions']=[dict(slot=slot,address=hex(fn),**location(items,fn)) for slot,fn in enumerate(struct.unpack('<12Q',read(handle,vtable,12*8)))]
        nearby=[]; exact=0; registered=0
        for slot in range(4096):
            candidate=struct.unpack('<Q',read(handle,manager+12472+slot*9296,8))[0]
            if not candidate: continue
            registered+=1; exact+=candidate==native
            if abs(candidate-native)<=4096:
                value={'slot':slot,'list':hex(candidate),'delta':candidate-native,'generation':struct.unpack('<Q',read(handle,manager+12472+slot*9296+16,8))[0]}
                try:
                    cv=struct.unpack('<Q',read(handle,candidate,8))[0]
                    value['vtable']=hex(cv); value['vtable_location']=location(items,cv)
                except Exception as error: value['vtable_error']=str(error)
                nearby.append(value)
        report['registered_count']=registered; report['exact_registered_count']=exact; report['nearby_registered']=nearby
        if thread.GetFrameAtIndex(0).GetPC()==context.rip:
            report['frames']=[dict(location(items,thread.GetFrameAtIndex(i).GetPC()),symbol=thread.GetFrameAtIndex(i).GetFunctionName()) for i in range(min(24,thread.GetNumFrames()))]
        else: report['frames_unavailable']='LLDB cached frame differs from native context'
        debugger.SetAsync(False)
        error=process.Detach(False)
        if error.Fail(): raise RuntimeError('detach failed '+str(error))
        report['detached']=process.GetState()==lldb.eStateDetached
        report['restored_after_detach']=read(handle,address,len(expected))==expected
        report['passed']=report['detached'] and report['restored_after_detach']
    except Exception as error:
        report['error']=str(error)
    finally:
        if process and process.IsValid() and process.GetState() not in (lldb.eStateDetached,lldb.eStateExited,lldb.eStateInvalid):
            if bp: process.GetTarget().BreakpointDelete(bp.GetID())
            if hit_tid:
                report['cleanup_restored_rip']=restore_boundary(helper,pid,hit_tid,address)
            report['cleanup_detach']=str(process.Detach(False))
        if handle and address:
            try: report['final_original_bytes']=read(handle,address,len(expected)).hex()
            except Exception as error: report['final_read_error']=str(error)
        if handle: K.CloseHandle(handle)
        with open(os.path.join(ROOT,'discovery','build','msfs-0.7.5-unknown-list-breakpoint.json'),'w') as output: json.dump(report,output,indent=2)
        print(json.dumps(report))

def read(handle, address, count):
    if not address or count < 1 or count > 32768:
        raise RuntimeError('read bounds')
    data = ctypes.create_string_buffer(count)
    done = ctypes.c_size_t()
    if not K.ReadProcessMemory(handle, address, data, count, ctypes.byref(done)) or done.value != count:
        raise RuntimeError('RPM failed %d at %x' % (ctypes.get_last_error(),address))
    return data.raw

def fixture(debugger):
    report = {'mode':'fixture','passed':False}
    process = None
    handle = None
    bp = None
    try:
        debugger.HandleCommand('settings set target.detach-on-error true')
        debugger.SetAsync(False)
        target = debugger.CreateTarget(os.path.join(ROOT,'discovery','build','debug-hw-fixture.exe'))
        bp = target.BreakpointCreateByName('taxi_debug_fixture')
        bp.SetOneShot(True)
        if bp.GetNumLocations() != 1: raise RuntimeError('fixture symbol missing')
        process = target.LaunchSimple(None,None,os.path.join(ROOT,'discovery','build'))
        if process.GetState() != lldb.eStateStopped: raise RuntimeError('fixture did not stop')
        handle = K.OpenProcess(0x410,False,process.GetProcessID())
        if not handle: raise RuntimeError('open fixture failed')
        thread = process.GetSelectedThread()
        report['stop_reason'] = thread.GetStopReason()
        if thread.GetStopReason() != lldb.eStopReasonBreakpoint: raise RuntimeError('not breakpoint')
        address = thread.GetFrameAtIndex(0).GetPC()
        target.BreakpointDelete(bp.GetID())
        expected = b'\x8b\x44\x24\x04\x83\xc0\x01\x59\xc3'
        report['bytes_before_detach'] = read(handle,address,len(expected)).hex()
        if read(handle,address,len(expected)) != expected: raise RuntimeError('original fixture bytes not restored')
        error = process.Detach(False)
        if error.Fail(): raise RuntimeError('detach failed '+error.GetCString())
        report['bytes_after_detach'] = read(handle,address,len(expected)).hex()
        report['detached'] = process.GetState() == lldb.eStateDetached
        report['passed'] = report['detached'] and read(handle,address,len(expected)) == expected
    except Exception as error:
        report['error'] = str(error)
    finally:
        if process and process.IsValid() and process.GetState() not in (lldb.eStateDetached,lldb.eStateExited,lldb.eStateInvalid):
            if bp: process.GetTarget().BreakpointDelete(bp.GetID())
            report['cleanup_detach'] = str(process.Detach(False))
        if handle: K.CloseHandle(handle)
        with open(os.path.join(ROOT,'discovery','build','debug-software-fixture.json'),'w') as output: json.dump(report,output,indent=2)
        print(json.dumps(report))

def __lldb_init_module(debugger, internal_dict):
    pass
