#include "scene_source_state.hpp"

namespace taxi_camera::source_state {
namespace {
bool valid(Key key) noexcept {
  return key.handle != 0 && key.generation != 0;
}
bool valid(Effect::Kind kind) noexcept {
  switch (kind) {
    case Effect::Kind::legacy_rt:
    case Effect::Kind::enhanced_rt:
    case Effect::Kind::other:
    case Effect::Kind::pass_other:
    case Effect::Kind::draw:
      return true;
  }
  return false;
}
}  // namespace

bool Recording::append(Effect effect) noexcept {
  if (invalid)
    return false;
  if (count > effects.size() || !valid(effect.key) || !valid(effect.kind)) {
    invalidate();
    return false;
  }
  if (count && effects[count - 1] == effect)
    return true;
  if (effect.kind == Effect::Kind::draw) {
    // Effects for other generation-qualified resources are independent. A draw
    // can repeat its last draw evidence until this same key changes state.
    for (std::size_t i = count; i != 0; --i) {
      if (effects[i - 1].key != effect.key)
        continue;
      if (effects[i - 1].kind == Effect::Kind::draw)
        return true;
      break;
    }
  }
  if (count == effects.size()) {
    overflowed = true;
    invalidate();
    return false;
  }
  effects[count++] = effect;
  return true;
}

bool Tracker::register_source(Key key, Model initial) noexcept {
  if (!valid(key) || (initial != Model::unknown && initial != Model::legacy_rt && initial != Model::enhanced_rt && initial != Model::other))
    return false;
  const Model retained = render_target_model(initial) ? initial : Model::unknown;
  Slot* free = nullptr;
  for (auto& slot : sources_) {
    if (slot.key == key)
      return true;
    if (slot.key.handle == key.handle) {
      slot = {key, {initial, false}, retained};
      return true;
    }
    if (!slot.key.handle && !free)
      free = &slot;
  }
  if (!free)
    return false;
  *free = {key, {initial, false}, retained};
  return true;
}

void Tracker::unregister_source(Key key) noexcept {
  if (!valid(key))
    return;
  for (auto& slot : sources_)
    if (slot.key == key) {
      slot = {};
      return;
    }
}

void Tracker::clear() noexcept {
  sources_ = {};
}

void Tracker::begin_batch() noexcept {
  for (auto& slot : sources_)
    slot.state.drawn = false;
}

bool Tracker::apply(const Recording& recording) noexcept {
  if (recording.invalid || recording.count > recording.effects.size()) {
    invalidate_all();
    return false;
  }
  for (std::size_t i = 0; i < recording.count; ++i) {
    const auto& effect = recording.effects[i];
    if (!valid(effect.key) || !valid(effect.kind)) {
      invalidate_all();
      return false;
    }
    for (auto& slot : sources_) {
      if (slot.key != effect.key)
        continue;
      switch (effect.kind) {
        case Effect::Kind::legacy_rt:
          slot.state = {Model::legacy_rt, false};
          slot.retained_rt = Model::legacy_rt;
          break;
        case Effect::Kind::enhanced_rt:
          slot.state = {Model::enhanced_rt, false};
          slot.retained_rt = Model::enhanced_rt;
          break;
        case Effect::Kind::other:
          slot.state = {Model::other, false};
          slot.retained_rt = Model::unknown;
          break;
        case Effect::Kind::pass_other:
          // Pass-state reports retire the live model; the bitmap's RT history remains.
          slot.state = {Model::other, false};
          break;
        case Effect::Kind::draw:
          slot.state.drawn = render_target_model(slot.state.model);
          break;
      }
      break;
    }
  }
  return true;
}

State Tracker::state(Key key) const noexcept {
  if (valid(key))
    for (const auto& slot : sources_)
      if (slot.key == key)
        return slot.state;
  return {};
}

void Tracker::invalidate_all() noexcept {
  for (auto& slot : sources_)
    slot.state = {};
}

void Tracker::retire_live_models() noexcept {
  for (auto& slot : sources_)
    if (slot.key.handle)
      slot.state = {Model::other, false};
}

unsigned Tracker::rearm_retained_rt() noexcept {
  unsigned count = 0;
  for (auto& slot : sources_) {
    if (!slot.key.handle || !render_target_model(slot.retained_rt) || slot.state.model == slot.retained_rt)
      continue;
    slot.state = {slot.retained_rt, false};
    ++count;
  }
  return count;
}

}  // namespace taxi_camera::source_state
