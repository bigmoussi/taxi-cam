#include "../../src/camera/view_resize_recovery.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
namespace ec = taxi_camera::engine_camera;
using Policy = taxi_camera::native_camera::ViewResizeRecovery;
using Action = Policy::Action;

unsigned checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::abort();
  }
}

constexpr ec::ManagerToken owner{7, 1};
constexpr Policy::Ids ids{1001, 1002};

Policy::Views ready_views() {
  Policy::Views views{};
  // The current recovery validator requires every snapshot slot to carry a
  // complete ready chain even when the third owned ID is zero. Match that
  // existing contract so this test isolates only Bitmap-settlement policy.
  for (unsigned i = 0; i < views.size(); ++i) {
    auto& view = views[i];
    view.complete = view.ready = true;
    view.status = ec::OwnedViewStatus::ready;
    view.view_address = 0x1000 + i * 0x100;
    view.node_address = 0x2000 + i * 0x100;
    view.camera_address = 0x3000 + i * 0x100;
    view.view_index = static_cast<int>(i);
    view.fov = 0.62f;
    view.flags[0] = 1;
    view.mode = 2;
    view.resource_present = true;
    view.output_dimensions = {900 + static_cast<int>(i), 700};
  }
  return views;
}

void obtain_resize(Policy& policy, Policy::Views& views, std::uint64_t& update) {
  const auto action = policy.observe(owner, ids, update++, views);
  if (action == Action::wait)
    require(policy.observe(owner, ids, update++, views) == Action::resize, "Closed recovery reaches resize authorization");
  else
    require(action == Action::resize, "Recovery remains immediately retryable after an unused authorization");
}
}  // namespace

int main() {
  {
    Policy policy;
    auto views = ready_views();
    std::uint64_t update = 1;
    require(policy.begin(owner, ids), "Stable mismatch test begins");
    for (unsigned retry = 1; retry <= Policy::MaximumStableOutputMismatches; ++retry) {
      obtain_resize(policy, views, update);
      policy.release_resize_authorization();
      require(policy.stable_output_mismatches() == retry, "Stable mismatch counter advances exactly once per fresh authorization");
      require(policy.failed() == (retry == Policy::MaximumStableOutputMismatches),
              "Only the bounded final stable mismatch marks recovery failed");
    }
    require(policy.observe(owner, ids, update, views) == Action::blocked, "Exhausted stable mismatch refuses further native attempts");
  }

  {
    Policy policy;
    auto views = ready_views();
    std::uint64_t update = 100;
    require(policy.begin(owner, ids), "Changing-size test begins");
    for (unsigned retry = 0; retry < Policy::MaximumStableOutputMismatches * 2; ++retry) {
      obtain_resize(policy, views, update);
      ++views[0].output_dimensions[0];
      // Publish the changed dimensions through a fresh observation before the
      // caller decides that the existing Bitmap still does not match its pane.
      require(policy.observe(owner, ids, update++, views) == Action::wait,
              "Consumed authorization waits while a new Bitmap size is observed");
      policy.release_resize_authorization();
      require(!policy.failed(), "A changing Bitmap is settling, not a stable incompatible output");
      require(policy.stable_output_mismatches() == 1, "A new observed Bitmap size restarts the stable mismatch proof");
    }
    obtain_resize(policy, views, update);
    require(policy.finish(owner, ids), "A settling output may still finish when the Bitmap eventually matches");
    require(!policy.pending() && !policy.failed() && policy.stable_output_mismatches() == 0,
            "Successful retained recovery clears the mismatch episode");
  }

  {
    Policy policy;
    auto views = ready_views();
    std::uint64_t update = 1000;
    require(policy.begin(owner, ids), "Transient-gap test begins");
    obtain_resize(policy, views, update);
    policy.release_resize_authorization();
    require(policy.stable_output_mismatches() == 1, "First stable mismatch is remembered");
    auto temporary = views;
    temporary[0] = {};
    temporary[0].status = ec::OwnedViewStatus::read_failed;
    require(policy.observe(owner, ids, update++, temporary) == Action::wait && !policy.failed(),
            "Temporary inspection gap remains retryable");
    require(policy.stable_output_mismatches() == 0, "Temporary inspection gap discards stale stability evidence");
    require(policy.observe(owner, ids, update++, views) == Action::wait, "Fresh closed evidence restarts after the transient gap");
    require(policy.observe(owner, ids, update++, views) == Action::resize, "Fresh later update can authorize recovery again");
  }

  std::printf("View resize settlement: PASS %u checks\n", checks);
}
