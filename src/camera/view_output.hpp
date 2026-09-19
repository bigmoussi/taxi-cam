#pragma once

#include "owned_view.hpp"
#include "view_resize.hpp"

namespace taxi_camera::native_camera {

// Output admission for opening a render gate.
//
// The renderer binds an active view's output through Material+520 -> Bitmap ->
// Bitmap+88 record -> record+16 wrapper -> wrapper+168 resource and does not
// null-check the record (retained RenderThreadProc faults read [record+0x10]
// with a null record). A ready camera chain (Node/Camera) says nothing about
// that output chain: pooled views keep the previous entry's material state,
// and the captured output routine may retain, replace or drop a Bitmap.
//
// A gate may therefore open only while the exact requested output is observed
// in the same inspection: mode2, the resource member present, the three view
// size pairs equal to the requested pane, and the Bitmap sized to that pane.
// This observes; it does not allocate, retain or prove GPU completion.
inline bool owned_view_output_ready(const engine_camera::OwnedViewSnapshot& view, const ViewDimensions& requested) noexcept {
  return view.complete && view.ready && view.status == engine_camera::OwnedViewStatus::ready && view.mode == 2 && view.resource_present &&
         view.resource_address != 0 && view.dimensions == requested && view.output_dimensions == requested[0];
}

}  // namespace taxi_camera::native_camera
