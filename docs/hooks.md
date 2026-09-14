# Win64 camera-update observer

The **vtable-slot observer hook** in `src/hooks/` is used by the camera integration. Its tests are in `tests/hooks/`. The hook itself contains no engine addresses, process discovery, remote memory operations or camera creation calls. The default observer is a no-op. It supplies the original RCX value to an optional observer, then tail-jumps to the original target without declaring that target's signature.

## Build and validate

From the repository root, run:

```powershell
& .\tests\hooks\test.ps1
```

The script uses the LLVM-MinGW compiler pinned in `dependencies.json`. It compiles with C++20, `-O2 -Wall -Wextra -Werror`, and disables AVX generation for these local sources. It creates only ignored files under `build/tests/hooks/`:

- `engine-hook.a`: hook implementation and assembly thunk for the parent integration to link.
- `engine-hook-validation.exe`: standalone executable using its own mock targets and read-only vtable page.
- `engine-hook-protection-validation.exe`: separate test-only build with deterministic `VirtualProtect` failure injection.
- `observer-unwind.txt`: LLVM's dump of the thunk object's Win64 unwind metadata.

The script runs the standalone executables. It does not launch, inspect or install anything into MSFS.

## Slot ownership and installation

`install(slot, expected_original, observer)` accepts one explicitly supplied, pointer-aligned, live slot and its exact expected original target. It checks the slot with `VirtualQuery`, refuses noncommitted/unreadable/guarded memory and executable slot pages, and checks that original/observer pointers refer to executable memory. These are memory-property checks, not function identity or calling-convention validation.

After an initial mismatch check, installation temporarily changes the slot page to `PAGE_READWRITE`, uses `InterlockedCompareExchangePointer` to install the thunk **only if the expected original still matches**, and attempts to restore the exact previous protection. It never writes instruction bytes, allocates executable memory or constructs a trampoline.

There can be **one successful installation per linked module instance**. A successful compare/exchange consumes that installation even if the subsequent protection restoration fails: another thread might already have entered the thunk. The saved original and observer pointers remain immutable afterward. Experiment enable/disable belongs in the observer's request state; it must not retarget this hook or replace the callback pointer.

`remove()` compares/exchanges **only this thunk** back to the saved original. A foreign replacement returns `slot_changed` and remains untouched. If that owner later restores this thunk, removal can be retried. Successful removal does not enable a second installation.

Each operation returns an explicit `Status`. `pointer_changed` reports whether its compare/exchange succeeded. `protection_restored` reports whether protection remained unchanged or was successfully restored. A restoration error returns **`protection_restore_failed` and the Windows error even when the slot exchange succeeded**. The caller must handle that condition; the API does not report success or imply rollback.

The original protection and its slot are retained until restoration succeeds, including after a compare/exchange mismatch. `restore_protection()` retries only that obligation, without exchanging the pointer. `install()` and `remove()` also retry it first and perform no pointer exchange while restoration remains unresolved. This prevents a later operation from treating a temporarily writable page as its original protection. A successful `restore_protection()` returns `protection_restored`; this also covers calls when no restoration is pending.

The caller must guarantee the slot allocation remains valid throughout query, protection changes and exchange, and for the entire duration of any pending protection recovery. These APIs do not prevent a different thread from unmapping or repurposing the memory. Installation/removal/recovery operations within this module are serialized; the observer never acquires their lock.

## Thunk state and unwind behavior

`observer_thunk.S` preserves:

- RAX, RCX, RDX and R8–R11.
- The low 128 bits of XMM0–XMM5.
- MXCSR, including its volatile exception-status bits.
- RFLAGS and the original entry RSP, return address, caller shadow space and stack arguments.

The observer follows the ordinary Win64 calling convention, which preserves the remaining nonvolatile registers. The thunk itself saves/restores RBP for its frame pointer. It allocates separate shadow space for the observer and tail-jumps through the immutable original pointer after restoring its state. Therefore the original function's return flows directly to its original caller.

RFLAGS are saved with `pushfq` before the stack allocation changes them. The unwind metadata describes that push as an eight-byte allocation, and the frame pointer describes the temporary stack change during flags restoration. Flags are restored before the final legal `LEA RSP`, `POP RBP`, indirect-jump epilogue. Microsoft's [x64 exception-handling documentation](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-170) documents `push_eflags` as `pushfq` plus an eight-byte unwind allocation; its [x64 prolog and epilog rules](https://learn.microsoft.com/en-us/cpp/build/prolog-and-epilog?view=msvc-170) describe the permitted epilogue shapes.

This is **Win64/SSE state coverage**, not full extended-processor-state preservation. Upper YMM/ZMM lanes, AVX-512 opmask registers and volatile x87 state are not saved. Disabling AVX in these sources does not constrain transitive observer callees or system libraries. An integration must establish that its target and observer are compatible with that limit; it cannot claim arbitrary unknown-signature/vector-state transparency. Microsoft documents the volatility of upper vector lanes and MXCSR status in its [x64 calling-convention reference](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170).

## Observer and module lifetime

The callback has the type `void (*)(void* original_rcx) noexcept`. Its RCX argument is an observation of the original call, not a validated camera-manager object. The callback must obey Win64 register/control-state requirements and must not allow C++ exceptions to escape. `noexcept` is not a barrier against access violations or other SEH faults.

A thread-local guard suppresses nested observer callbacks. A reentrant call through the hook still reaches the original target exactly once, but skips the callback on that thread. The guard is not a lifetime, synchronization or engine-thread permission mechanism.

**Removal does not wait for in-flight invocations.** The hook module, saved original's module, observer code and all callback-owned state must remain alive until the integration establishes quiescence. A thread may already hold the thunk address or may be paused inside it when the slot is restored. A third-party hook may also retain this thunk in its own chain. No DLL unload is permitted until those conditions have been resolved. Pinning and quiescence belong to the parent integration; this module deliberately provides no unsafe reset or unload shortcut.

## What the standalone validation proves

The host allocates a data page, populates a mock vtable slot, and makes it read-only. It exercises refusal of null/unaligned/unmapped slots, a data original pointer and an expected-original mismatch; successful installation; a foreign replacement during removal; successful removal; and refusal of reinstallation.

Assembly mock calls test **128 different states** while the observer deliberately overwrites every covered volatile GPR, XMM0–XMM5 and MXCSR status. Each case checks original RCX delivery, unchanged register/vector/stack inputs and entry RSP, arithmetic RFLAGS, MXCSR, exactly one original invocation, and scalar/XMM return values. It also checks reentrant observer suppression and behavior before installation and after removal.

`RtlVirtualUnwind` checks **ten instruction locations** spanning prolog boundaries, established frame, temporary flags-restoration push and epilogue. Real stack captures from inside the callback must unwind across the thunk to the mock caller. The mock caller/target assembly exists only in the validation executable; the hook library contains no assumed engine target prototype.

A separate validation build injects protection failures after successful installation, successful removal and a simulated compare/exchange race, plus failure of the initial writable-protection change. It checks pending recovery survives retries, pointer exchanges are blocked until recovery succeeds, foreign pointers remain untouched, the exact original read-only protection is restored, and only a successful install exchange consumes installation. Each scenario runs in a fresh process. The production library has no failure-injection dependency.

The validation does not establish engine object identity, simulator thread/phase permission, engine exception behavior, extended-vector compatibility, CFG/XFG/CET compatibility in the simulator, camera output, or GPU-resource lifetime. Those limits remain separate from the local register, unwind and recovery checks.
