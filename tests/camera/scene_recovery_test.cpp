#include "../../src/camera/scene_recovery.hpp"
#include "../../src/shared/scene_demand.hpp"
#include "view_retirement_test.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
namespace ec = taxi_camera::engine_camera;
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}
struct Engine {
  unsigned creates = 0, erases = 0;
  bool allow_erase = false;
  static bool initialize(void*, ec::DescriptorStorage& descriptor) noexcept {
    descriptor.bytes.fill(0);
    return true;
  }
  static ec::EntryId create(void* context, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    return 100 + ++static_cast<Engine*>(context)->creates;
  }
  static bool erase(void* context, ec::ManagerToken, ec::EntryId) noexcept {
    auto& engine = *static_cast<Engine*>(context);
    ++engine.erases;
    return engine.allow_erase;
  }
  ec::EngineCallbacks callbacks() { return {this, initialize, create, erase}; }
};

struct CreationEngine {
  enum class Mode { ready, temporary_pose, pool_refused, fatal_pose, descriptor_contract, native_failure, second_initializer };
  Mode mode = Mode::temporary_pose;
  unsigned initializations = 0, creates = 0, erases = 0;
  bool allow_erase = false;
  static bool initialize(void* context, ec::DescriptorStorage& descriptor) noexcept {
    auto& engine = *static_cast<CreationEngine*>(context);
    ++engine.initializations;
    if (engine.mode == Mode::temporary_pose || engine.mode == Mode::pool_refused || engine.mode == Mode::fatal_pose ||
        (engine.mode == Mode::second_initializer && engine.initializations == 2))
      return false;
    descriptor.bytes.fill(0);
    if (engine.mode == Mode::descriptor_contract)
      descriptor.bytes[0] = 1;
    return true;
  }
  static ec::EntryId create(void* context, ec::ManagerToken, const ec::DescriptorStorage&) noexcept {
    auto& engine = *static_cast<CreationEngine*>(context);
    ++engine.creates;
    return engine.mode == Mode::native_failure ? 0 : 100 + engine.creates;
  }
  static bool erase(void* context, ec::ManagerToken, ec::EntryId) noexcept {
    auto& engine = *static_cast<CreationEngine*>(context);
    ++engine.erases;
    return engine.allow_erase;
  }
  ec::EngineCallbacks callbacks() { return {this, initialize, create, erase}; }
};

void initial_pose_deferral() {
  constexpr ec::ManagerToken manager{11, 4};
  constexpr std::uint64_t revision = 7;
  CreationEngine engine;
  ec::PairController pair;
  SceneRecovery recovery;
  recovery.start();
  taxi_camera::standalone::ScenePrewarm prewarm;
  require(prewarm.observe(50, true, false, {}, false), "Prewarm did not begin its accepted request");
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::failed &&
              pair.snapshot().failure == ec::Failure::initializer_failed && engine.creates == 0 && engine.erases == 0,
          "First initializer refusal did not consume creation without native calls");
  require(defer_initial_pose_failure(pair, manager, recovery, true, 0, revision, revision, 100),
          "Temporary first initializer refusal was not deferred");
  const auto waiting = pair.snapshot();
  require(waiting.state == ec::State::disabled && waiting.failure == ec::Failure::none && recovery.pending() &&
              recovery.reason() == SceneStopReason::inspection_unavailable && engine.initializations == 1 && engine.creates == 0 &&
              engine.erases == 0,
          "Deferral published a failed state or invoked native callbacks while clearing the empty request");
  require(prewarm.observe(100, true, false, {}, waiting.state == ec::State::failed || waiting.state == ec::State::blocked) &&
              prewarm.pending() && prewarm.phase() == taxi_camera::standalone::ScenePrewarm::Phase::preparing,
          "Deferred empty creation permanently parked background prewarm");
  require(!recovery.retry(2099, waiting, true) && !recovery.retry(2100, waiting, false) && engine.creates == 0 && engine.erases == 0,
          "Deferral bypassed delay or fresh pose before creating");
  require(recovery.retry(2100, waiting, true) && recovery.attempts() == 1, "Validated retry did not use the existing bounded policy");
  engine.mode = CreationEngine::Mode::ready;
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::active && engine.creates == 2 &&
              engine.erases == 0,
          "Deferred startup did not create exactly one pair after fresh validation");

  // Other refusals must retain their original failure and ownership. In
  // particular, a failed native create returns zero without proving that no
  // native operation ran, and a second initializer may already own an ID.
  for (const auto mode : {CreationEngine::Mode::pool_refused, CreationEngine::Mode::fatal_pose, CreationEngine::Mode::descriptor_contract,
                          CreationEngine::Mode::native_failure, CreationEngine::Mode::second_initializer}) {
    CreationEngine refused;
    refused.mode = mode;
    ec::PairController failed;
    SceneRecovery policy;
    policy.start();
    failed.request_independent_pose();
    require(failed.process_update(manager, refused.callbacks()), "Refusal fixture did not execute");
    const auto before = failed.snapshot();
    const auto creates = refused.creates, erases = refused.erases;
    const bool temporary = mode != CreationEngine::Mode::pool_refused && mode != CreationEngine::Mode::fatal_pose;
    const unsigned created = mode == CreationEngine::Mode::second_initializer ? 1 : 0;
    require(!defer_initial_pose_failure(failed, manager, policy, temporary, created, revision, revision, 100),
            "Pool, fatal pose, descriptor, native or partial-pair failure was automatically deferred");
    require(failed.snapshot().state == before.state && failed.snapshot().failure == before.failure &&
                failed.snapshot().owned_ids == before.owned_ids && !policy.pending() && refused.creates == creates &&
                refused.erases == erases,
            "Rejected deferral altered ownership, recovery or native calls");
  }

  for (unsigned cancellation = 0; cancellation < 3; ++cancellation) {
    CreationEngine refused;
    ec::PairController failed;
    SceneRecovery policy;
    policy.start();
    failed.request_independent_pose();
    require(failed.process_update(manager, refused.callbacks()), "Cancelled request fixture did not execute");
    if (cancellation == 0) {
      policy.stop();
      failed.request_disable();
    } else if (cancellation == 2) {
      policy.failed(SceneStopReason::identity_refused, 99);
    }
    const auto sequence = policy.sequence();
    require(!defer_initial_pose_failure(failed, manager, policy, true, 0, revision, revision + (cancellation != 2), 100),
            "A stale request or fatal identity refusal was revived by pose deferral");
    require(!policy.pending() && policy.sequence() == sequence && refused.creates == 0 && refused.erases == 0 &&
                failed.snapshot().request_pending == (cancellation == 0),
            "Cancelled deferral consumed the newer Stop or changed failure policy");
  }

  {
    CreationEngine removed;
    removed.mode = CreationEngine::Mode::second_initializer;
    removed.allow_erase = true;
    ec::PairController partial;
    SceneRecovery policy;
    policy.start();
    partial.request_independent_pose();
    require(partial.process_update(manager, removed.callbacks()) && !partial.snapshot().owned_ids[0] && removed.creates == 1 &&
                removed.erases == 1 && partial.snapshot().failure == ec::Failure::initializer_failed,
            "Confirmed partial-pair cleanup fixture did not execute");
    require(!defer_initial_pose_failure(partial, manager, policy, true, 1, revision, revision, 100) && !policy.pending() &&
                removed.creates == 1 && removed.erases == 1,
            "Removed IDs hid a native creation from the initial-pose-only deferral guard");
  }

  CreationEngine unstable;
  ec::PairController empty;
  SceneRecovery bounded;
  bounded.start();
  for (unsigned attempt = 0; attempt <= SceneRecovery::maximum_retries; ++attempt) {
    empty.request_independent_pose();
    require(empty.process_update(manager, unstable.callbacks()), "Repeated transient fixture did not execute");
    const auto now = 100 + attempt * 3000ull;
    const bool deferred = defer_initial_pose_failure(empty, manager, bounded, true, 0, revision, revision, now);
    if (attempt < SceneRecovery::maximum_retries)
      require(deferred && bounded.retry(now + SceneRecovery::retry_delay_ms, empty.snapshot(), true),
              "An allowed transient attempt did not follow the existing retry policy");
    else
      require(!deferred && !bounded.pending() && !bounded.retry(now + 99999, empty.snapshot(), true) &&
                  empty.snapshot().state == ec::State::failed,
              "Repeated transient initializer refusals exceeded the three-retry limit");
  }
  require(bounded.attempts() == SceneRecovery::maximum_retries && unstable.creates == 0 && unstable.erases == 0,
          "Bounded empty deferrals reset attempts or performed native allocation/cleanup");
}

void manager_failure_recovery() {
  constexpr ec::ManagerToken manager{9, 3};
  Engine engine;
  ec::PairController pair;
  SceneRecovery recovery;
  recovery.start();
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()), "Manager recovery initial pair");
  const auto original = pair.snapshot().owned_ids;

  // A temporary pair failure already queued cleanup when the next manager
  // inspection becomes unavailable. Neither failed inspection may call native
  // erase. The original IDs must remain owned until a fresh valid update.
  recovery.failed(SceneStopReason::inspection_unavailable, 100);
  pair.request_disable();
  recovery.failed(SceneStopReason::inspection_unavailable, 460);
  pair.request_disable();
  require(recovery.pending() && recovery.attempts() == 0 && pair.snapshot().owned_ids == original && engine.erases == 0,
          "Temporary manager inspection consumed ownership or disabled pair recovery");
  require(!recovery.retry(5000, pair.snapshot(), true), "Manager recovery bypassed unprocessed cleanup");
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::cleanup_pending,
          "Validated update did not preserve unconfirmed cleanup");
  require(!recovery.retry(5000, pair.snapshot(), true) && engine.creates == 2, "Unconfirmed IDs permitted recreation");
  engine.allow_erase = true;
  require(pair.process_update(manager, engine.callbacks()) && !pair.snapshot().owned_ids[0] && !pair.snapshot().owned_ids[1],
          "Fresh guarded cleanup did not confirm absence");
  require(!recovery.retry(5000, pair.snapshot(), false), "Manager retry bypassed fresh aircraft pose");
  require(recovery.retry(5000, pair.snapshot(), true), "Temporary pair then manager failure did not recover");
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && engine.creates == 4 && pair.snapshot().owned_ids != original,
          "Recovery did not create exactly one new pair");

  // A manager-only temporary failure must also queue cleanup for an otherwise
  // active pair. Exercise every remaining retry, with repeated unavailable
  // samples between attempts; these must never refill the retry budget.
  for (unsigned attempt = 2; attempt <= SceneRecovery::maximum_retries; ++attempt) {
    const auto retained = pair.snapshot().owned_ids;
    const auto creates = engine.creates, erases = engine.erases;
    const auto at = std::uint64_t(attempt) * 10000;
    for (unsigned sample = 0; sample < 4; ++sample) {
      recovery.failed(SceneStopReason::inspection_unavailable, at + sample);
      pair.request_disable();
    }
    require(recovery.pending() && recovery.attempts() == attempt - 1 && pair.snapshot().owned_ids == retained &&
                engine.creates == creates && engine.erases == erases,
            "Failed manager inspection called native callbacks or reset retry accounting");
    require(pair.process_update(manager, engine.callbacks()), "Fresh manager-only cleanup update");
    require(recovery.retry(at + 3000, pair.snapshot(), true) && recovery.attempts() == attempt,
            "Confirmed manager-only cleanup did not use one bounded retry");
    pair.request_independent_pose();
    require(pair.process_update(manager, engine.callbacks()) && engine.creates == creates + 2,
            "Manager-only retry duplicated or omitted a pair");
  }
  pair.request_disable();
  require(pair.process_update(manager, engine.callbacks()), "Final bounded cleanup");
  for (unsigned sample = 0; sample < 16; ++sample)
    recovery.failed(SceneStopReason::inspection_unavailable, 50000 + sample);
  require(!recovery.pending() && !recovery.retry(99999, pair.snapshot(), true) && recovery.attempts() == SceneRecovery::maximum_retries,
          "Repeated manager faults revived an exhausted retry budget");

  recovery.start();
  recovery.failed(SceneStopReason::inspection_unavailable, 100);
  recovery.failed(SceneStopReason::identity_refused, 460);
  const auto refused_sequence = recovery.sequence();
  for (const auto later : {SceneStopReason::pose_invalid, SceneStopReason::creation_failed, SceneStopReason::exception,
                           SceneStopReason::inspection_unavailable, SceneStopReason::owned_entry_absent})
    recovery.failed(later, 1000);
  require(recovery.reason() == SceneStopReason::identity_refused && !recovery.pending() && recovery.sequence() == refused_sequence &&
              !recovery.retry(99999, pair.snapshot(), true),
          "Later unavailable inspection downgraded an actual manager identity mismatch");
}

void manager_failure_before_calibration() {
  constexpr ec::ManagerToken manager{19, 6};
  constexpr std::uint64_t revision = 8;
  CreationEngine engine;
  engine.mode = CreationEngine::Mode::ready;
  ec::PairController pair;
  SceneRecovery recovery;
  recovery.start();
  // The initial manager failure consumes Start before capture_pose calibrates.
  recovery.failed(SceneStopReason::inspection_unavailable, 100);
  pair.request_disable();
  require(!initial_retry_calibration_allowed(recovery, pair.snapshot(), true, false, revision, revision),
          "Calibration bypassed an unprocessed controller request");
  require(pair.process_update(manager, engine.callbacks()), "Fresh manager could not settle the empty disable");
  const auto clean = pair.snapshot();
  require(clean.state == ec::State::disabled && !clean.request_pending && engine.initializations == 0 && engine.creates == 0,
          "Empty manager-failure cleanup performed native work");
  // Provider contract: fresh public camera+aircraft samples can require three
  // calibration observations while valid remains false. The old valid-only
  // gate could never enter capture_pose and therefore never became retryable.
  unsigned pose_observations = 0;
  for (unsigned pass = 0; pass < 3; ++pass) {
    require(initial_retry_calibration_allowed(recovery, clean, true, false, revision, revision),
            "Fresh uncalibrated startup could not progress its read-only pose prerequisite");
    const bool pose_usable = ++pose_observations == 3;
    const bool retry = recovery.retry(3000 + 250 * pass, clean, pose_usable);
    require(retry == pose_usable && recovery.attempts() == (pose_usable ? 1u : 0u),
            "Calibration consumed retry budget before a usable pose");
    require(engine.initializations == 0 && engine.creates == 0 && engine.erases == 0,
            "Read-only calibration entered a native camera callback");
  }
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::active && engine.creates == 2 &&
              engine.erases == 0 && recovery.attempts() == 1,
          "Calibrated manager retry did not create exactly one guarded pair");

  recovery.start();
  recovery.failed(SceneStopReason::inspection_unavailable, 100);
  require(!initial_retry_calibration_allowed(recovery, clean, false, false, revision, revision) &&
              !initial_retry_calibration_allowed(recovery, clean, true, true, revision, revision) &&
              !initial_retry_calibration_allowed(recovery, clean, true, false, revision, revision + 1) &&
              !initial_retry_calibration_allowed(recovery, pair.snapshot(), true, false, revision, revision),
          "Stale telemetry, suspension, changed request or owned pair authorized initial calibration");
  for (unsigned field = 0; field < 7; ++field) {
    auto invalid = clean;
    switch (field) {
      case 0:
        invalid.owner = manager;
        break;
      case 1:
        invalid.owned_ids[1] = 101;
        break;
      case 2:
        invalid.creation_pending = true;
        break;
      case 3:
        invalid.request_pending = true;
        break;
      case 4:
        invalid.failure = ec::Failure::initializer_contract;
        break;
      case 5:
        invalid.blocked = ec::Blocked::invalid_manager;
        break;
      case 6:
        invalid.state = ec::State::failed;
        break;
    }
    require(!initial_retry_calibration_allowed(recovery, invalid, true, false, revision, revision),
            "Unsettled ownership/failure authorized calibration recovery");
  }
  recovery.failed(SceneStopReason::identity_refused, 200);
  require(!initial_retry_calibration_allowed(recovery, clean, true, false, revision, revision),
          "Fatal identity failure authorized calibration recovery");
  recovery.start();
  recovery.failed(SceneStopReason::inspection_unavailable, 100);
  require(initial_retry_calibration_allowed(recovery, clean, true, false, revision, revision), "Stop-race fixture was not pending");
  recovery.stop();
  require(!recovery.retry(99999, clean, true) && !initial_retry_calibration_allowed(recovery, clean, true, false, revision, revision + 1),
          "Stop arriving during calibration rearmed native creation");
}

void stale_local_calibration_restarts_creation() {
  // Issue 54: outside_local_calibration_radius must not latch pose_invalid.
  // Map it onto the existing retryable inspection path so cleanup plus
  // calibration can recreate the pair (manual deactivate/activate is fallback).
  require(outside_local_calibration_radius("outside_local_calibration_radius"),
          "Exact outside-radius sample detail was not recognized");
  require(!outside_local_calibration_radius("outside_calibration_radius") && !outside_local_calibration_radius("pose_invalid") &&
              !outside_local_calibration_radius(nullptr) && !outside_local_calibration_radius(""),
          "Unrelated pose errors were treated as a stale local lock");
  require(stop_reason_for_body_pose_failure("outside_local_calibration_radius") == SceneStopReason::inspection_unavailable &&
              retryable_scene_stop(stop_reason_for_body_pose_failure("outside_local_calibration_radius")),
          "Stale local calibration did not select the retryable restart path");
  require(stop_reason_for_body_pose_failure("invalid_body_basis") == SceneStopReason::pose_invalid &&
              !retryable_scene_stop(stop_reason_for_body_pose_failure("invalid_body_basis")),
          "Ordinary pose failures must still latch without automatic recreate");
  require(!temporary_pose_unavailable("outside_local_calibration_radius"),
          "Stale local calibration must not retain a closed pair forever without reset");

  constexpr ec::ManagerToken manager{23, 9};
  constexpr std::uint64_t revision = 11;
  CreationEngine engine;
  engine.mode = CreationEngine::Mode::ready;
  ec::PairController pair;
  SceneRecovery recovery;
  recovery.start();
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::active && engine.creates == 2,
          "Active parked-pair fixture was not created");
  const auto original = pair.snapshot().owned_ids;

  // Active-pair re-arm after a long sector: retire through retryable recovery.
  recovery.failed(stop_reason_for_body_pose_failure("outside_local_calibration_radius"), 100);
  require(recovery.pending() && recovery.reason() == SceneStopReason::inspection_unavailable && recovery.attempts() == 0,
          "Outside-radius stop latched instead of requesting a bounded recreate");
  pair.request_disable();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::cleanup_pending &&
              pair.snapshot().owned_ids == original,
          "Stale-calibration restart lost ownership before confirmed cleanup");
  engine.allow_erase = true;
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::disabled && !pair.snapshot().owned_ids[0] &&
              !pair.snapshot().owned_ids[1],
          "Confirmed cleanup did not clear the departure pair");
  const auto clean = pair.snapshot();
  // Same gate used after manager failure: uncalibrated arrival may progress the
  // read-only pose path before the bounded retry consumes an attempt.
  require(initial_retry_calibration_allowed(recovery, clean, true, false, revision, revision),
          "Arrival recalibration was refused after the outside-radius restart");
  require(!recovery.retry(2099, clean, false) && engine.creates == 2, "Retry bypassed a usable pose after relocation");
  require(recovery.retry(2100, clean, true) && recovery.attempts() == 1, "Validated outside-radius restart did not recreate");
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::active && engine.creates == 4 &&
              pair.snapshot().owned_ids != original,
          "Outside-radius recovery did not create exactly one fresh arrival pair");

  // Ordinary pose_invalid remains a latched stop that needs an explicit Start.
  SceneRecovery latched;
  latched.start();
  latched.failed(stop_reason_for_body_pose_failure("invalid_body_basis"), 5000);
  require(latched.reason() == SceneStopReason::pose_invalid && !latched.pending() && !latched.retry(99999, clean, true),
          "Non-radius pose failure became automatically retryable");
}

void partial_creation_failure_stays_latched() {
  constexpr ec::ManagerToken manager{13, 5};
  CreationEngine engine;
  engine.mode = CreationEngine::Mode::second_initializer;
  ec::PairController pair;
  SceneRecovery recovery;
  recovery.start();
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::cleanup_pending &&
              pair.snapshot().owned_ids[0] && !pair.snapshot().owned_ids[1] && engine.creates == 1,
          "Partial creation fixture did not retain its unconfirmed owned ID");
  const auto original = pair.snapshot().owned_ids;
  recovery.failed(SceneStopReason::creation_failed, 100);
  const auto sequence = recovery.sequence();
  pair.request_disable();
  const auto try_automatic_creation = [&](std::uint64_t now) {
    if (recovery.retry(now, pair.snapshot(), true)) {
      pair.request_independent_pose();
      pair.process_update(manager, engine.callbacks());
    }
  };

  const auto before_cleanup_erases = engine.erases;
  recovery.failed(SceneStopReason::inspection_unavailable, 200);
  try_automatic_creation(5000);
  require(recovery.reason() == SceneStopReason::creation_failed && !recovery.pending() && recovery.sequence() == sequence &&
              pair.snapshot().owned_ids == original && engine.creates == 1 && engine.erases == before_cleanup_erases,
          "Transient inspection replaced the initial failure or touched ownership before guarded cleanup");
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::cleanup_pending &&
              pair.snapshot().owned_ids == original && engine.creates == 1,
          "Failed erase lost the partially created ID or created another pair");

  engine.allow_erase = true;
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::disabled &&
              !pair.snapshot().owned_ids[0] && !pair.snapshot().owned_ids[1] && engine.creates == 1,
          "Fresh cleanup did not confirm the partially created ID absent");
  const auto confirmed_erases = engine.erases;
  for (const auto later : {SceneStopReason::inspection_unavailable, SceneStopReason::owned_entry_absent, SceneStopReason::identity_refused,
                           SceneStopReason::exception}) {
    recovery.failed(later, 6000);
    try_automatic_creation(99999);
    require(recovery.reason() == SceneStopReason::creation_failed && !recovery.pending() && recovery.sequence() == sequence &&
                recovery.attempts() == 0 && pair.snapshot().state == ec::State::disabled && engine.creates == 1 &&
                engine.erases == confirmed_erases,
            "Later failure revived automatic allocation or replaced the original terminal cause after confirmed cleanup");
  }
  recovery.stop();
  require(recovery.reason() == SceneStopReason::explicit_stop && !recovery.requested() && !recovery.pending(),
          "Explicit Stop did not clear the latched creation failure");
  recovery.start();
  require(recovery.reason() == SceneStopReason::none && recovery.requested() && !recovery.pending() && recovery.attempts() == 0,
          "Explicit Start did not reset the latched creation failure");
  engine.mode = CreationEngine::Mode::ready;
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()) && pair.snapshot().state == ec::State::active && engine.creates == 3 &&
              engine.erases == confirmed_erases,
          "Explicit Start did not create exactly one fresh pair after confirmed cleanup");
}
}  // namespace

int main() {
  test_view_retirement();
  manager_failure_recovery();
  manager_failure_before_calibration();
  initial_pose_deferral();
  stale_local_calibration_restarts_creation();
  partial_creation_failure_stays_latched();
  SceneRecovery recovery;
  ec::Snapshot clean;
  require(!recovery.retry(9999, clean, true), "No initial invented request");
  for (const auto* error : {"telemetry_busy", "aircraft_telemetry_stale", "camera_telemetry_stale", "not_initialized"})
    require(temporary_pose_unavailable(error), "Temporary telemetry errors retain a closed pair");
  for (const auto* error :
       {"", "invalid_basis", "outside_calibration_radius", "outside_local_calibration_radius", "identity_mismatch"})
    require(!temporary_pose_unavailable(error), "Identity and numeric failures remain fatal");
  require(!temporary_pose_unavailable(nullptr), "Null error never classified temporary");
  for (const auto reason :
       {SceneStopReason::none, SceneStopReason::explicit_stop, SceneStopReason::identity_refused, SceneStopReason::pose_invalid,
        SceneStopReason::creation_failed, SceneStopReason::exception, SceneStopReason::capture_stalled}) {
    recovery.start();
    recovery.failed(reason, 10);
    require(!recovery.pending() && !recovery.retry(99999, clean, true), "Fatal failure cannot retry");
  }
  for (const auto reason : {SceneStopReason::inspection_unavailable, SceneStopReason::owned_entry_absent}) {
    recovery.start();
    recovery.failed(reason, 100);
    require(recovery.pending() && recovery.reason() == reason, "Recoverable reason is recorded");
    const auto original_sequence = recovery.sequence();
    for (const auto repeated_at : {460u, 1100u, 2099u})
      recovery.failed(reason, repeated_at);
    require(recovery.sequence() == original_sequence && recovery.attempts() == 0,
            "Repeated pending inspection failures restarted the recovery episode");
    require(!recovery.retry(99, clean, true), "Clock regression refuses retry");
    require(!recovery.retry(2099, clean, true), "Retry delay boundary");
    require(!recovery.retry(2100, clean, false), "No stale pose retry");
    for (unsigned field = 0; field < 7; ++field) {
      auto incomplete = clean;
      if (field == 0)
        incomplete.owned_ids[0] = 1;
      if (field == 1)
        incomplete.owned_ids[1] = 2;
      if (field == 2)
        incomplete.request_pending = true;
      if (field == 3)
        incomplete.creation_pending = true;
      if (field == 4)
        incomplete.state = ec::State::cleanup_pending;
      if (field == 5)
        incomplete.failure = ec::Failure::manager_destroyed;
      if (field == 6)
        incomplete.blocked = ec::Blocked::manager_mismatch;
      require(!recovery.retry(2100, incomplete, true), "Unconfirmed cleanup or invalid owner blocks retry");
    }
    require(recovery.retry(2100, clean, true), "Confirmed clean state retries after the delay");
    require(!recovery.retry(2100, clean, true), "Same failure cannot queue duplicate creation");
  }

  // Exercise the actual ownership controller through a failed erase and retry.
  Engine engine;
  ec::PairController pair;
  constexpr ec::ManagerToken manager{7, 1};
  recovery.start();
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()), "Initial controller update");
  const auto original = pair.snapshot().owned_ids;
  require(original[0] && original[1] && engine.creates == 2, "Initial owned pair exists");
  // A missing pose closes gates in the caller, without issuing a disable.
  require(temporary_pose_unavailable("aircraft_telemetry_stale"), "Stale pose suspends");
  require(pair.process_update(manager, engine.callbacks()), "Suspended controller update");
  require(pair.snapshot().owned_ids == original && engine.erases == 0 && engine.creates == 2,
          "Temporary pose gap retains IDs without repeated creation or erase");
  recovery.failed(SceneStopReason::resolution_changed, 90);
  require(!recovery.pending() && !recovery.retry(5000, pair.snapshot(), true), "Resolution change cannot recreate retained views");
  const auto resolution_sequence = recovery.sequence();
  recovery.resumed_retained_resolution();
  require(recovery.reason() == SceneStopReason::none && recovery.requested() && recovery.sequence() == resolution_sequence + 1,
          "Retained resolution recovery preserves demand and logs the restore");
  recovery.failed(SceneStopReason::owned_entry_absent, 100);
  pair.request_disable();
  require(pair.process_update(manager, engine.callbacks()), "First cleanup update");
  require(pair.snapshot().state == ec::State::cleanup_pending && pair.snapshot().owned_ids == original,
          "Unconfirmed erase preserves both IDs");
  require(!recovery.retry(5000, pair.snapshot(), true), "No retry while either ID remains");
  engine.allow_erase = true;
  require(pair.process_update(manager, engine.callbacks()), "Confirmed cleanup update");
  require(recovery.retry(5000, pair.snapshot(), true), "Retry after actual controller confirms absence");
  pair.request_independent_pose();
  require(pair.process_update(manager, engine.callbacks()), "Retry creation update");
  require(engine.creates == 4 && pair.snapshot().owned_ids[0] != original[0], "Exactly one new pair created");

  recovery.failed(SceneStopReason::inspection_unavailable, 6000);
  recovery.stop();
  pair.request_disable();
  require(pair.process_update(manager, engine.callbacks()), "Fresh OFF cleanup");
  require(!recovery.requested() && !recovery.pending() && !recovery.retry(99999, pair.snapshot(), true),
          "Explicit OFF cancels pending recovery immediately");
  require(engine.creates == 4, "Fresh OFF never recreates");
  recovery.start();
  for (unsigned attempt = 1; attempt <= SceneRecovery::maximum_retries; ++attempt) {
    recovery.failed(SceneStopReason::owned_entry_absent, attempt * 3000);
    require(recovery.retry(attempt * 3000 + 2000, clean, true) && recovery.attempts() == attempt, "Bounded retry accepted");
  }
  recovery.failed(SceneStopReason::owned_entry_absent, 20000);
  require(!recovery.pending() && !recovery.retry(99999, clean, true), "Retry storm is bounded");
  recovery.start();
  require(recovery.attempts() == 0 && recovery.reason() == SceneStopReason::none, "Explicit new Start resets retry budget");
  recovery.failed(SceneStopReason::inspection_unavailable, 1000);
  require(recovery.retry(3000, clean, true), "Capture stall uses confirmed cleanup policy");
  recovery.capture_progress(4000);
  recovery.capture_progress(20000);
  require(recovery.attempts() == 1, "Long capture gaps cannot refill the retry budget");
  for (std::uint64_t time = 21000; time < 30000; time += 1000)
    recovery.capture_progress(time);
  require(recovery.attempts() == 1, "Healthy interval must complete before refill");
  recovery.capture_progress(30000);
  require(recovery.attempts() == 0, "Ten seconds of advancing captures refill recovery budget");
  recovery.failed(SceneStopReason::identity_refused, 31000);
  for (std::uint64_t time = 32000; time < 50000; time += 1000)
    recovery.capture_progress(time);
  require(recovery.reason() == SceneStopReason::identity_refused && !recovery.pending(), "Capture progress cannot clear identity failure");
  std::printf("Scene recovery: PASS %u checks\n", checks);
}
