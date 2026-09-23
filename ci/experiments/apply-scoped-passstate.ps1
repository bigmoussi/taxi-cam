[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

function Replace-Exact([string]$Path, [string]$Old, [string]$New, [string]$Label) {
  $full = Join-Path $repo $Path
  $text = [IO.File]::ReadAllText($full)
  $count = ([regex]::Matches($text, [regex]::Escape($Old))).Count
  if ($count -ne 1) { throw "${Label}: expected exactly one match, got $count in $Path" }
  [IO.File]::WriteAllText($full, $text.Replace($Old, $New), [Text.UTF8Encoding]::new($false))
}

Replace-Exact 'src/graphics/scene_source_state.hpp' @'
  bool append(Effect effect) noexcept;
  void invalidate() noexcept { invalid = true; }
'@ @'
  bool append(Effect effect) noexcept;
  // A pass-local refusal drops evidence only for keys named by this recording.
  // It cannot narrow an already invalid, overflowing or malformed recording.
  // Unknown commands/barriers must continue to use invalidate().
  bool invalidate_named_sources() noexcept;
  void invalidate() noexcept { invalid = true; }
'@ 'scene source declaration'

Replace-Exact 'src/graphics/scene_source_state.cpp' @'
bool Tracker::register_source(Key key, Model initial) noexcept {
'@ @'
bool Recording::invalidate_named_sources() noexcept {
  if (invalid || overflowed || count > effects.size()) {
    invalidate();
    return false;
  }
  for (std::size_t i = 0; i < count; ++i)
    if (!valid(effects[i].key) || !valid(effects[i].kind)) {
      invalidate();
      return false;
    }
  // Replacing in place also works at capacity and preserves every generation.
  // No GPU state is mutated until this recording is actually submitted.
  for (std::size_t i = 0; i < count; ++i)
    effects[i].kind = Effect::Kind::other;
  return true;
}

bool Tracker::register_source(Key key, Model initial) noexcept {
'@ 'scene source implementation'

Replace-Exact 'src/graphics/scene_capture_manager.cpp' @'
    stats_.last_invalidation_reasons = reasons;
    item->source_effects.invalidate();
    if (global)
      touch_sources(*item);
'@ @'
    stats_.last_invalidation_reasons = reasons;
    // A pass-state refusal does not describe a transition of every camera on
    // the device. Preserve unrelated sources, but discard all evidence named
    // by this recording. Never narrow alias/split/unknown-work/reset failures,
    // including those combined with PassState, or a previously invalid log.
    using namespace engine_hook::render_boundary;
    constexpr std::uint32_t pass_reasons = InvalidationPassBegin | InvalidationPassState;
    if ((reasons & InvalidationPassState) && !(reasons & ~pass_reasons) &&
        item->source_effects.invalidate_named_sources()) {
      if (item->source_effects.count)
        touch_sources(*item);
      return;
    }
    item->source_effects.invalidate();
    if (global)
      touch_sources(*item);
'@ 'capture-manager scoped PassState policy'

$sourceStateTest = @'
void pass_local_invalidation() {
  Tracker tracker;
  require(tracker.register_source(nose, Model::legacy_rt) && tracker.register_source(tail, Model::enhanced_rt), "register pass pair");
  Recording bystander;
  require(bystander.invalidate_named_sources() && tracker.apply(bystander), "empty pass refusal");
  state_is(tracker, nose, Model::legacy_rt, false);
  state_is(tracker, tail, Model::enhanced_rt, false);
  auto affected = record({{nose, Kind::legacy_rt}, {nose, Kind::draw}});
  require(affected.invalidate_named_sources(), "scope named pass");
  state_is(tracker, nose, Model::legacy_rt, false);
  require(tracker.apply(affected), "submit scoped refusal");
  state_is(tracker, nose, Model::other, false);
  state_is(tracker, tail, Model::enhanced_rt, false);
  require(tracker.rearm_retained_rt() == 0, "watchdog must not revive refused source");
  require(tracker.apply(record({{nose, Kind::draw}})), "draw after refused pass");
  state_is(tracker, nose, Model::other, false);
  require(tracker.apply(record({{nose, Kind::legacy_rt}, {nose, Kind::draw}})), "fresh absolute evidence restores capture");
  state_is(tracker, nose, Model::legacy_rt, true);
  require(tracker.register_source({nose.handle, nose.generation + 1}, Model::legacy_rt), "replacement source");
  require(tracker.apply(affected), "old generation replay");
  state_is(tracker, {nose.handle, nose.generation + 1}, Model::legacy_rt, false);
  Recording full;
  for (std::size_t i = 0; i < Recording::capacity; ++i)
    require(full.append({{i + 1, 1}, Kind::draw}), "fill scoped recording");
  require(full.invalidate_named_sources() && !full.invalid && full.count == Recording::capacity, "full recording remains bounded");
  for (std::size_t i = 0; i < full.count; ++i)
    require(full.effects[i].kind == Kind::other, "full recording retains no RT or draw proof");
  for (unsigned failure = 0; failure < 5; ++failure) {
    Recording bad;
    if (failure == 0) bad.invalidate();
    if (failure == 1) bad.overflowed = true;
    if (failure == 2) bad.count = Recording::capacity + 1;
    if (failure == 3) { bad.count = 1; bad.effects[0] = {{0, 1}, Kind::draw}; }
    if (failure == 4) { bad.count = 1; bad.effects[0] = {nose, static_cast<Kind>(99)}; }
    require(!bad.invalidate_named_sources() && bad.invalid, "unsafe recording must remain globally invalid");
    require(!tracker.apply(bad), "unsafe submission refused");
    state_is(tracker, tail, Model::unknown, false);
  }
}

'@
Replace-Exact 'tests/graphics/scene_source_state_test.cpp' 'void actual_submission_order() {' ($sourceStateTest + 'void actual_submission_order() {') 'source-state PassState regression'
Replace-Exact 'tests/graphics/scene_source_state_test.cpp' @'
int main() {
  native_creation_state();
'@ @'
int main() {
  pass_local_invalidation();
  native_creation_state();
'@ 'source-state regression entry'

$managerTest = @'
  {
    Commands bystander;
    bystander.initialize(device.p);
    constexpr std::uint64_t BystanderGeneration = Generation + 9;
    require(manager->register_command_list(bystander.list.p, DeviceKey, BystanderGeneration), "Register unrelated pass list");
    auto* owner = manager->device(DeviceKey);
    auto* item = manager->list(bystander.list.p);
    require(owner && item, "Pass-state fixture identities");
    const taxi_camera::source_state::Key left{reinterpret_cast<std::uint64_t>(sources[0].p), ids[0]};
    const taxi_camera::source_state::Key right{reinterpret_cast<std::uint64_t>(sources[1].p), ids[1]};
    const auto before_left = owner->source_states.state(left).model;
    const auto before_right = owner->source_states.state(right).model;
    manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true, Boundary::InvalidationPassState);
    require(!item->source_effects.invalid && !item->source_touched, "Unrelated pass must not prepare global camera wipe");
    ID3D12CommandList* batch = bystander.list.p;
    require(manager->before_submission(bystander.queue.p, 1, &batch) == 0, "Unrelated pass remains unrelated at submission");
    require(owner->source_states.state(left).model == before_left && owner->source_states.state(right).model == before_right,
            "Unrelated pass preserved both camera states");
    require(item->source_effects.append({left, taxi_camera::source_state::Effect::Kind::draw}), "Name one camera");
    manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true,
                                        Boundary::InvalidationPassBegin | Boundary::InvalidationPassState);
    require(!item->source_effects.invalid && item->source_touched && item->source_effects.count == 1 &&
                item->source_effects.effects[0].kind == taxi_camera::source_state::Effect::Kind::other,
            "Named source loses evidence without global invalidation");
    require(owner->source_states.state(left).model == before_left, "Recording cannot mutate submitted state");
    for (const auto reason : {Boundary::InvalidationAliasOrDiscard, Boundary::InvalidationSplitBarrier,
                              Boundary::InvalidationUnobservedWork, Boundary::InvalidationResetFailed,
                              Boundary::InvalidationBarrierBatch, Boundary::InvalidationObserverDisabled}) {
      manager->successful_reset(bystander.list.p, BystanderGeneration);
      manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true, Boundary::InvalidationPassState | reason);
      require(item->source_effects.invalid && item->source_touched, "Mixed unsafe reason must retain global refusal");
    }
    manager->successful_reset(bystander.list.p, BystanderGeneration);
    item->source_effects.invalidate();
    manager->invalidate_source_recording(bystander.list.p, BystanderGeneration, true, Boundary::InvalidationPassState);
    require(item->source_effects.invalid && item->source_touched, "Pass refusal must not narrow prior uncertainty");
    manager->destroy_command_list(bystander.list.p, BystanderGeneration);
  }
'@
Replace-Exact 'tests/graphics/scene_queue_tail_test.cpp' @'
  manager->set_gpu_timing_enabled(true);
  DrawFixture draw;
'@ ("  manager->set_gpu_timing_enabled(true);`n" + $managerTest + "  DrawFixture draw;`n") 'manager PassState regression'

Write-Host 'Scoped PassState safety fix applied to performance branch.'
