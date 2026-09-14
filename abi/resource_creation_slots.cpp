// Compile-only public COM slot/call contract against the pinned Windows header.
#define CINTERFACE
#include <d3d12.h>
#include <stddef.h>

static_assert(sizeof(void*) == 8);
#define CREATION_SLOT(name, index) static_assert(offsetof(ID3D12Device10Vtbl, name) == (index) * sizeof(void*))
CREATION_SLOT(CreateCommittedResource, 27);
CREATION_SLOT(CreatePlacedResource, 29);
CREATION_SLOT(CreateReservedResource, 30);
CREATION_SLOT(CreateCommittedResource1, 53);
CREATION_SLOT(CreateReservedResource1, 55);
CREATION_SLOT(CreateCommittedResource2, 69);
CREATION_SLOT(CreatePlacedResource1, 70);
CREATION_SLOT(CreateCommittedResource3, 76);
CREATION_SLOT(CreatePlacedResource2, 77);
CREATION_SLOT(CreateReservedResource2, 78);
#undef CREATION_SLOT

extern "C" HRESULT creation_committed(ID3D12Device10* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
                                       const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                       REFIID iid, void** out) {
  return device->lpVtbl->CreateCommittedResource(device, heap, flags, desc, state, clear, iid, out);
}
extern "C" HRESULT creation_placed(ID3D12Device10* device, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
                                    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
  return device->lpVtbl->CreatePlacedResource(device, heap, offset, desc, state, clear, iid, out);
}
extern "C" HRESULT creation_reserved(ID3D12Device10* device, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                      const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
  return device->lpVtbl->CreateReservedResource(device, desc, state, clear, iid, out);
}
extern "C" HRESULT creation_committed1(ID3D12Device10* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
                                        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                        ID3D12ProtectedResourceSession* protected_session, REFIID iid, void** out) {
  return device->lpVtbl->CreateCommittedResource1(device, heap, flags, desc, state, clear, protected_session, iid, out);
}
extern "C" HRESULT creation_reserved1(ID3D12Device10* device, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                       const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* protected_session,
                                       REFIID iid, void** out) {
  return device->lpVtbl->CreateReservedResource1(device, desc, state, clear, protected_session, iid, out);
}
extern "C" HRESULT creation_committed2(ID3D12Device10* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
                                        const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                        ID3D12ProtectedResourceSession* protected_session, REFIID iid, void** out) {
  return device->lpVtbl->CreateCommittedResource2(device, heap, flags, desc, state, clear, protected_session, iid, out);
}
extern "C" HRESULT creation_placed1(ID3D12Device10* device, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
                                     D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
  return device->lpVtbl->CreatePlacedResource1(device, heap, offset, desc, state, clear, iid, out);
}
extern "C" HRESULT creation_committed3(ID3D12Device10* device, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
                                        const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
                                        ID3D12ProtectedResourceSession* protected_session, UINT32 count, DXGI_FORMAT* formats,
                                        REFIID iid, void** out) {
  return device->lpVtbl->CreateCommittedResource3(device, heap, flags, desc, layout, clear, protected_session, count, formats, iid, out);
}
extern "C" HRESULT creation_placed2(ID3D12Device10* device, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
                                     D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, UINT32 count, DXGI_FORMAT* formats,
                                     REFIID iid, void** out) {
  return device->lpVtbl->CreatePlacedResource2(device, heap, offset, desc, layout, clear, count, formats, iid, out);
}
extern "C" HRESULT creation_reserved2(ID3D12Device10* device, const D3D12_RESOURCE_DESC* desc, D3D12_BARRIER_LAYOUT layout,
                                       const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* protected_session,
                                       UINT32 count, DXGI_FORMAT* formats, REFIID iid, void** out) {
  return device->lpVtbl->CreateReservedResource2(device, desc, layout, clear, protected_session, count, formats, iid, out);
}
