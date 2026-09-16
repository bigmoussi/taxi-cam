# Extra-view lighting contract

Runway and taxiway lights can be faint or absent in the native taxi-camera images while aircraft lights remain visible. This distinction does not establish that all lighting or emissive rendering is absent. Exposure is a separate display-conversion issue.

The current source path reads only `Material_Z+520`, material slot 0, named `VIEWPORT_MATERIAL_DIFFUSE VP%d`. `src/camera/owned_view.cpp` resolves `P+144`/`E+80` to that material, then the bitmap backend record and native wrapper. It does not read or combine slot 9 at material+664, named `VIEWPORT_MATERIAL_ADD_DIFFUSE VP%d`.

The full saved function in `build/msfs-1.8.16.0-camera-view-types.json` proves:

- `66812808` selects material+664.
- `66812836..66812857` forms effective flags from P+48, OR-global RVA176053040 and clear-global RVA176053024.
- `66812860..66812876` permits the ADD_DIFFUSE allocation only when effective bit49 is set and bit18 is clear.
- `66812732..66812771` scales both output dimensions by the float at RVA130842980 before allocation.
- `66813353/66813364` associates the new bitmap with material slot9 using `69763648`.

These instructions prove allocation conditions, not what draws into that bitmap. No captured writer, shader label, or render-dispatch consumer identifies it as an airport-light, emissive, bloom, or final-composite buffer. Its name alone is insufficient to assign one of those meanings.

The saved wrapper constructor `64894368` copies its R9 descriptor's 56 bytes into wrapper+96 at `64894674..64894705`. The recorded caller supplies the same descriptor to native resource allocation; another caller obtains that descriptor through resource virtual slot80. This permits reading the cached D3D12 dimensions/format without executing COM. Such a read does not acquire a resource reference or prove current GPU state.

## Live metadata

`build/msfs-view-material-lighting-first.json` is a one-shot read-only capture of PID31444. Same-user identity, executable/module file identity, MSFS1.8.16.0 tuple and all29 existing code fingerprints passed. It consumed2192 object bytes and47535 image bytes, with zero read failures. All observed identity/descriptor fields were reread; both flag samples matched. Primary index0 was separately resolved through the existing aircraft metadata reader.

| View | Output dimensions | P+48 flags | Slot0 | Slot9 ADD_DIFFUSE |
| --- | --- | --- | --- | --- |
| Primary, index0 | 5120x1369 | `0x001ae003ff0329de` | Absent from this material | 640x171, format10, one mip/layer/sample |
| Nose-size view, index1 | 768x255 | `0x0019e013ff2220ef` | 768x255, format26 | Absent |
| Tail-size view, index2 | 768x504 | `0x0019e013ff2220ef` | 768x504, format26 | Absent |

Format10 is `R16G16B16A16_FLOAT`; format26 is `R11G11B10_FLOAT`. The three present bitmap handles each had generation1 matching their control records. Published resource ordinals establish equality only inside this snapshot; they are not addresses, durable identities, or graphics registry generations. The two extra views are identified here by their exact dimensions and pool indices, not by a newly acquired ownership token.

Primary bit49 is set and both extra-view bit49 values are clear; bit18 is clear in all three. Primary ADD_DIFFUSE is one eighth of the primary output dimensions after integer truncation. Those facts explain its allocation difference, but still do not prove the missing airport-light contribution lives there.

Setup `17642240` ORs a global pair plus bits5/21/36, clears bits8/10/11/16/30 at `17642887`, then optionally ORs the 16-byte constant at130434112 when E+72 is nonzero. Production mode2 uses descriptor+44=1, which supplies E+72. None of these bit labels has been established as an airport-light control. Activation bit0 is independently verified; its meaning must not be extended to other bits.

A GPU capture of the primary ADD_DIFFUSE image at a known night view, with live resource registration and queue/fence ownership, is required to determine whether it contains the missing light contribution. Any owned-view implementation also requires a verified render-consumer/flag contract and source lifetime before enabling or combining it. Copying the primary image into the taxi feeds would use the wrong camera projection. The available evidence does not provide a production lighting fix.

## Diagnostic validation

`view_material_build.ps1` builds the independent reader and runs 91 synthetic checks, including null/stale handles, byte-aligned controls, duplicate views, changed identity/descriptor rejection, independently changing activation flags, exact reads and resource equality ordinals. A complete synthetic eight-view/two-slot trace uses 4,944 bytes. It also checks wrong-process refusal before any simulator read. The executables link the runtime statically, and the script verifies that their PE imports contain only Windows/UCRT DLLs before starting both validation processes hidden.
