"""Only a disposable local host; native context helper is loaded into LLDB."""
import ctypes, json, os
import lldb
import debug_one_shot

class Registers(ctypes.Structure):
    _fields_ = [(name,ctypes.c_uint64) for name in ('rip','rsp','rsi','rdi','rbx','r10')] + [('error',ctypes.c_uint32),('flags',ctypes.c_uint32)]

def run(debugger):
    report={'passed':False}; process=None; handle=None; bp=None; expected=None; helper=None; advanced=False
    try:
        root=debug_one_shot.ROOT
        helper=ctypes.WinDLL(os.path.join(root,'discovery','build','debug-native-context-fixture.dll'),use_last_error=True)
        helper.read_debug_thread.argtypes=[ctypes.c_ulong,ctypes.c_ulong,ctypes.POINTER(Registers)]
        helper.read_debug_thread.restype=ctypes.c_int
        helper.rewind_debug_thread.argtypes=[ctypes.c_ulong,ctypes.c_ulong,ctypes.c_uint64,ctypes.c_uint64]
        helper.rewind_debug_thread.restype=ctypes.c_int
        helper.advance_debug_fixture_thread.argtypes=[ctypes.c_ulong,ctypes.c_ulong,ctypes.c_uint64]
        helper.advance_debug_fixture_thread.restype=ctypes.c_int
        debugger.HandleCommand('settings set target.detach-on-error true')
        debugger.SetAsync(False)
        target=debugger.CreateTarget(os.path.join(root,'discovery','build','debug-hw-fixture.exe'))
        bp=target.BreakpointCreateByName('taxi_debug_fixture'); bp.SetOneShot(True)
        process=target.LaunchSimple(None,None,os.path.join(root,'discovery','build'))
        if process.GetState()!=lldb.eStateStopped: raise RuntimeError('fixture did not stop')
        thread=process.GetSelectedThread(); regs=Registers()
        if not helper.read_debug_thread(process.GetProcessID(),thread.GetThreadID(),ctypes.byref(regs)):
            raise RuntimeError('native context failed %d'%regs.error)
        expected=thread.GetFrameAtIndex(0).GetPC()
        report.update(native_rip=hex(regs.rip),lldb_rip=hex(expected),native_rsp=hex(regs.rsp),thread_id=thread.GetThreadID())
        if regs.rip!=expected: raise RuntimeError('initial native context mismatch')
        if not helper.advance_debug_fixture_thread(process.GetProcessID(),thread.GetThreadID(),expected): raise RuntimeError('fixture advance failed')
        advanced=True
        if not helper.read_debug_thread(process.GetProcessID(),thread.GetThreadID(),ctypes.byref(regs)) or regs.rip!=expected+1:
            raise RuntimeError('raw breakpoint+1 context not observed')
        report['raw_breakpoint_rip']=hex(regs.rip)
        if helper.rewind_debug_thread(process.GetProcessID(),thread.GetThreadID(),expected+1,expected-1): raise RuntimeError('nonunit rewind accepted')
        if helper.rewind_debug_thread(process.GetProcessID()+1,thread.GetThreadID(),expected+1,expected): raise RuntimeError('wrong process accepted')
        if helper.rewind_debug_thread(process.GetProcessID(),thread.GetThreadID(),expected+2,expected+1): raise RuntimeError('wrong current RIP accepted')
        if not helper.rewind_debug_thread(process.GetProcessID(),thread.GetThreadID(),expected+1,expected): raise RuntimeError('guarded rewind failed')
        advanced=False
        if not helper.read_debug_thread(process.GetProcessID(),thread.GetThreadID(),ctypes.byref(regs)) or regs.rip!=expected:
            raise RuntimeError('rewind reread mismatch')
        report['rewind_verified']=True
        target.BreakpointDelete(bp.GetID())
        handle=debug_one_shot.K.OpenProcess(0x410,False,process.GetProcessID())
        original=bytes.fromhex('8b44240483c00159c3')
        if debug_one_shot.read(handle,expected,len(original))!=original: raise RuntimeError('byte restoration before detach')
        error=process.Detach(False)
        if error.Fail(): raise RuntimeError('detach failed '+str(error))
        report['restored_after_detach']=debug_one_shot.read(handle,expected,len(original))==original
        report['passed']=regs.rip==expected and report['restored_after_detach'] and process.GetState()==lldb.eStateDetached
    except Exception as error: report['error']=str(error)
    finally:
        if process and process.IsValid() and process.GetState() not in (lldb.eStateDetached,lldb.eStateExited,lldb.eStateInvalid):
            if advanced and helper and expected:
                report['cleanup_rewind']=bool(helper.rewind_debug_thread(process.GetProcessID(),process.GetSelectedThread().GetThreadID(),expected+1,expected))
            if bp: process.GetTarget().BreakpointDelete(bp.GetID())
            report['cleanup_detach']=str(process.Detach(False))
        if handle: debug_one_shot.K.CloseHandle(handle)
        with open(os.path.join(debug_one_shot.ROOT,'discovery','build','debug-native-context-fixture.json'),'w') as output: json.dump(report,output,indent=2)
        print(json.dumps(report))
