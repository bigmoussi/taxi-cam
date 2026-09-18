#pragma once
#include <cstddef>
#include <vector>
#include "../graphics/pfd_submission_proof.hpp"
#include "../graphics/pfd_target_detector.hpp"
#include "../graphics/scene_runtime.hpp"
namespace taxi_camera::standalone {
inline constexpr const char* pfd_gpu_operation_name(unsigned slot) noexcept {
  switch (slot) {
    case 0:
      return "unknown_gpu_work";
    case 12:
      return "draw";
    case 14:
      return "dispatch";
    case 15:
      return "copy_buffer";
    case 16:
      return "copy_texture";
    case 17:
      return "copy_resource";
    case 18:
      return "copy_tiles";
    case 19:
      return "resolve";
    case 47:
      return "clear_depth";
    case 48:
      return "clear_render_target";
    case 49:
      return "clear_uav_uint";
    case 50:
      return "clear_uav_float";
    case 52:
      return "begin_query";
    case 53:
      return "end_query";
    case 54:
      return "resolve_query";
    case 60:
      return "atomic_copy_uint";
    case 61:
      return "atomic_copy_uint64";
    case 64:
      return "resolve_region";
    case 66:
      return "write_buffer";
    case 68:
      return "render_pass";
    case 72:
      return "build_raytracing";
    case 73:
      return "emit_raytracing";
    case 74:
      return "copy_raytracing";
    case 76:
      return "dispatch_rays";
    case 79:
      return "dispatch_mesh";
    default:
      return "unclassified_gpu_work";
  }
}
enum class DisplaySubmissionOutcome : unsigned {
  not_ready,
  unverified_close,
  invalid_batch,
  registry_busy,
  generation_changed,
  unknown_list,
  unclosed_list,
  incomplete_proof,
  no_display_exit,
  patch_not_ready,
  no_selected_exit,
  unavailable_target,
  calibration_budget,
  planned,
  count
};
inline constexpr const char* display_submission_outcome_name(DisplaySubmissionOutcome value) noexcept {
  constexpr const char* names[]{"not_ready",        "unverified_close",   "invalid_batch",      "registry_busy",   "generation_changed",
                                "unknown_list",     "unclosed_list",      "incomplete_proof",   "no_display_exit", "patch_not_ready",
                                "no_selected_exit", "unavailable_target", "calibration_budget", "planned"};
  return static_cast<unsigned>(value) < static_cast<unsigned>(DisplaySubmissionOutcome::count) ? names[static_cast<unsigned>(value)]
                                                                                               : "invalid_outcome";
}
struct GraphicsStatus {
  bool ready{};
  // draws counts fully observed recordings; idle retains only per-resource
  // activity required by the complete PFD detector inventory.
  std::uint64_t device{}, resources{}, lists{}, draws{}, hook_failures{}, clear_states{};
  const char* error = "not_started";
  std::uint64_t selected_draws{}, selected_rt_metadata{}, selected_rt_callbacks{}, selected_pending_matches{};
  std::uint64_t selected_view_resolved{}, selected_view_rejected{}, copy_attempts{}, copy_rejected{};
  const char* copy_error = "not_attempted";
  std::array<std::uint64_t, 32> selected_exit_scopes{};
  std::uint64_t calibration_clears{};
  std::uint64_t selected_exit_base{}, selected_exit_nonbase{}, selected_exit_split{};
  std::uint64_t fallback_attempts{}, fallback_stamps{}, fallback_query_refused{}, fallback_state_refused{};
  std::uint64_t preferred_copy_attempts{}, preferred_copy_stamps{}, preferred_copy_no_proof{};
  const char* preferred_copy_reason = "not_attempted";
  std::uint64_t dynamic_depth_bias_calls{}, dynamic_strip_cut_calls{}, sample_position_calls{};
  std::uint64_t recording_end_draws{}, shader_deferred{}, close_forward_refused{};
  const char* target_detection = "warming_up";
  bool observing{};
  std::uint64_t observation_epoch{}, observation_invalidations{};
  // Opt-in hot-path diagnostics: misses equal actual registry acquisitions.
  std::uint64_t list_lookup_calls{}, list_cache_hits{}, list_registry_lookups{};
  std::uint64_t idle_state_bypasses{}, idle_callback_bypasses{};
  std::uint64_t queue_patch_plans{};
  bool queue_close_verified{};
  std::array<std::uint64_t, static_cast<unsigned>(DisplaySubmissionOutcome::count)> queue_outcomes{};
  std::array<std::uint64_t, static_cast<unsigned>(PfdSubmissionProof::Refusal::count)> queue_proof_refusals{};
  unsigned queue_last_proof_flags{};
  std::array<std::uint64_t, 81> queue_prefix_blockers{};
};
bool initialize_graphics() noexcept;
// Resolve a reported device's optional COM proxy chain before native hooks or
// owned GPU work. Isolated validation supplies a hardware/WARP device; this
// never calls the simulator or telemetry.
bool initialize_graphics(IUnknown*) noexcept;
bool graphics_ready() noexcept;
// Include bounded warmup, rendering and calibration demand. Idle keeps native
// resource/descriptor lifetime and display activity discovery, plus retirement
// of previously recorded/submitted work. Source models stay continuously
// observed. PFD state omitted while idle needs real successful Reset to resume.
void set_graphics_observation_demand(bool enabled) noexcept;
void set_graphics_diagnostics_enabled(bool enabled) noexcept;
GraphicsStatus graphics_status() noexcept;
std::vector<PfdTargetObservation> pfd_inventory();
// Control-thread only. Turns off late-attach barrier/copy/OM extras after the
// profile's complete display set has distinct RTV associations, or after a short empty-list timeout. A filled
// list keeps association a little longer so stamps/calibration can light.
// Not called from recording hooks.
void service_live_backfill(std::uint64_t now, std::size_t inventory_count) noexcept;
// Control thread: configure exact display patch encoding and geometry before
// scene_runtime::service prepares GPU work for future submission boundaries.
void service_display_patches() noexcept;
bool assign_targets(std::uint64_t left, std::uint64_t right) noexcept;
void set_target_mask(unsigned mask) noexcept;
void set_calibration(unsigned mask, unsigned budget) noexcept;
std::array<std::uint64_t, 2> target_ids() noexcept;
void set_aircraft_profile(std::uint32_t id) noexcept;
void discover_pfds(std::uint64_t now) noexcept;
}  // namespace taxi_camera::standalone
