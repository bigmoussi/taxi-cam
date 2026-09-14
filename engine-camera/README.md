# Camera entry pair controller

This is a **mock-tested contract layer**, with no simulator addresses, module discovery, engine calls, hook installation or rendering implementation. A future adapter can supply callbacks after establishing its build, ABI, thread, manager lifetime and removal-verification contracts. This directory does not install or run anything in MSFS.

## Build and test

From the repository root:

```powershell
& .\engine-camera\build.ps1
```

The script uses the parent probe's pinned LLVM-MinGW compiler, with C++20, `-O2 -Wall -Wextra -Werror`. It creates the ignored `build/engine-camera.a` and `build/engine-camera-test.exe`, then runs the standalone mock tests. The library contains no Windows or MSFS API dependency. Its C++ interface must be compiled compatibly with its eventual consumer; these tests do not establish compatibility with a private engine ABI.

## Descriptor backing storage

The observed initializer accesses fields through offset 195, establishing a **minimum observed extent of 196 bytes**. `DescriptorStorage` supplies 256 zeroed bytes, aligned to 16 bytes. Both padding and alignment are local conservative choices, **not a recovered engine `sizeof`, complete layout or ABI guarantee**.

For each entry, the controller first invokes the injected initializer on a fresh store. `pack_mode_zero` then requires both name members to be empty and inline: byte 0 and DWORD 32 are zero, and byte 64 and DWORD 192 are zero. It writes DWORD 40=0, byte 44=1, and the caller's 16 opaque key bytes at offsets 48..63. It leaves other bytes unchanged. Invalid name fields are refused without overwriting them. The adapter must not supply an initializer that allocates resources requiring an unprovided destructor; the captured initializer only resets the observed inline fields.

The two keys remain opaque bytes. The controller does not parse GUID strings, assume a `cameras.cfg` relationship, derive camera poses, set dimensions or promise that creation yields a rendered view. A nonzero ID can describe an entry whose setup is pending. See the [captured entry contract](../discovery/camera-entry-contract.md) for the observed field data flow and unresolved engine contracts.

## Requests and update execution

`request_enable(PairKeys)` and `request_disable()` publish into a last-wins mailbox protected by a mutex. `snapshot()` returns a copied status. These methods never call an engine callback and can be used by the UI thread.

Only `process_update(ManagerToken, EngineCallbacks)` performs initialization, creation or erasure. The integration must call it from the approved update observer while the manager is live. An atomic guard refuses concurrent or reentrant processing; it does not itself prove the caller is on the right engine thread. Callbacks run without the mailbox mutex held, so a callback or another thread can publish a request for the next update. Requests arriving after the current update takes its mailbox snapshot are processed on a later update.

The injected callback interface is an adapter contract, not a declaration of private functions:

| Callback | Required meaning |
| --- | --- |
| `initialize(context, storage) -> bool` | Complete the initializer on the supplied backing store; false is a consumed creation failure |
| `create(context, manager, descriptor) -> uint64_t` | Return the new entry ID; zero means no owned ID was returned |
| `erase(context, manager, id) -> bool` | Request erasure and return true only after confirming that exact ID is absent |

The observed native erasure consumes a manager in RCX and the 64-bit ID **by value** in RDX. No status return has been established. It can return early while a renderer dependency is absent. Consequently the injected bool is a separate adapter confirmation, not a cast of the native return register. An adapter must not claim successful cleanup just because its call returned.

Every update does bounded work: at most two initializer calls, two create calls and three erase calls when replacing an old pair and rolling back a new first entry. There is no loop waiting for engine readiness. Snapshot state `active` means the controller owns two IDs; it does not mean either camera has completed setup or rendered a frame.

## Ownership, rollback and failures

Creation is ordered. The controller owns each nonzero ID immediately after its callback returns. If the second initialization or creation fails, it requests removal of the first ID. A duplicate returned ID is a contract failure and is erased only once. Full pairs are cleaned in reverse creation order.

Confirmed-absent IDs are immediately forgotten and never erased again. Unconfirmed IDs remain owned and cleanup is retried, at most once per remaining ID on each later update. Because an unconfirmed erase may still have removed an entry, a pair that has begun cleanup can never be treated as active again. An enable received during cleanup finishes removing the old ownership before attempting a new pair.

An initializer or create failure consumes its enable request. Once rollback finishes, repeated updates remain failed and perform no more creation calls. Only a new explicit enable request can retry creation. A new disable clears the failure and finishes any outstanding cleanup. Re-enabling an unchanged, active pair is idempotent.

`ManagerToken` contains an opaque nonzero identity and a nonzero generation. The integration must change the generation when a manager address or identity is reused. While any ID remains owned, an update with a different token is blocked without engine calls. It cannot erase old IDs through a newly supplied manager.

`acknowledge_manager_destroyed(exact_token)` is an explicit observer-side lifecycle event. It is permitted only after the engine owner has destroyed that exact manager lifetime. It discards those invalidated IDs, clears queued creation requests and latches a failure, with no callback. A mismatching token is refused. It must not be used to bypass unsuccessful removal of a live manager's entries.

There is no automatic destructor cleanup. The integration must request disable and continue approved updates until no IDs remain, or supply the verified manager-destruction event. Callback contexts and managers must outlive each processing call. The controller cannot repair a callback that creates an entry but returns an incorrect zero ID, lies about erasure, dereferences a stale manager or fails partway through an undocumented engine operation.

## Validation

Tests verify exact packing and untouched bytes; empty-name refusal; opaque key order; last-wins requests; active-pair idempotence; ordered creation and reverse cleanup; first/second initialization and creation failures; duplicate-ID handling; rollback retention and recovery; no automatic creation retries; partial cleanup; explicit replacement; manager-generation refusal and destruction acknowledgement; missing callbacks; reentry; and UI request publication during an in-flight update. Every mock callback checks that it runs on the designated update thread.

These tests establish local state-machine behavior. They do not establish a valid engine key, safe private call, scene-camera image, renderer synchronization or GPU handoff.
