#include <cassert>
#include <cstdio>
#include <limits>

#include "../src/taxi_button_routes.hpp"

int main() {
  taxi_camera::TaxiButtonIntent intent;
  assert(intent.observe(1, false, true, true).buttons == 0);  // No invented initial ON.
  assert(intent.observe(10, true, true, false).buttons == 1);
  auto signal = intent.observe(100, false, false, false);
  assert(signal.buttons == 1 && signal.held && !signal.timed_out);
  assert(intent.observe(108, true, true, false).buttons == 1);  // Observed eight-ms gap.
  assert(intent.observe(200, false, false, false).buttons == 1);
  assert(intent.observe(206, true, true, false).buttons == 1);  // Observed six-ms gap.
  assert(intent.observe(300, false, false, false).held);
  signal = intent.observe(2299, false, false, false);
  assert(signal.buttons == 1 && signal.held && !signal.timed_out);
  signal = intent.observe(2300, false, false, false);
  assert(signal.buttons == 0 && !signal.held && signal.timed_out);
  assert(intent.observe(2400, true, true, true).buttons == 3);    // Freshness recovery is automatic.
  assert(intent.observe(2401, true, false, false).buttons == 0);  // Fresh OFF is immediate.
  assert(intent.observe(2500, false, true, true).buttons == 0);   // Invalid values cannot turn on.
  assert(intent.observe(2600, true, false, true).buttons == 2);
  assert(intent.observe(2700, false, false, false).buttons == 2);
  signal = intent.observe(2699, false, false, false);
  assert(signal.buttons == 0 && signal.timed_out);              // Clock regression closes.
  assert(intent.observe(2701, false, false, false).timed_out);  // Only a fresh response clears expiry.
  intent.reset();
  assert(intent.observe(2800, false, true, true).buttons == 0);  // Manual override clears old intent.

  taxi_camera::TaxiButtonRoutes routes;
  assert(routes.targets[0] == 0 && routes.targets[1] == 0);
  assert(routes.active_mask(true, true, true) == 0);
  assert(!routes.matches(0, 3) && !routes.matches(101, 3));
  assert(!routes.assign(0, 0) && !routes.assign(2, 101));
  assert(!routes.assign(std::numeric_limits<unsigned>::max(), 101));

  assert(routes.assign(0, 101));
  assert(routes.active_mask(true, true, true) == 1);
  assert(routes.active_mask(true, false, true) == 0);
  assert(routes.assign(0, 101));  // Reassigning the same side is harmless.
  assert(!routes.assign(1, 101));
  assert(routes.targets[0] == 101 && routes.targets[1] == 0);
  assert(routes.assign(1, 202));

  assert(routes.active_mask(true, false, false) == 0);
  assert(routes.active_mask(true, true, false) == 1);
  assert(routes.active_mask(true, false, true) == 2);
  assert(routes.active_mask(true, true, true) == 3);
  for (unsigned buttons = 0; buttons < 4; ++buttons)
    assert(routes.active_mask(false, (buttons & 1) != 0, (buttons & 2) != 0) == 0);
  assert(routes.matches(101, 1) && !routes.matches(202, 1));
  assert(routes.matches(202, 2) && !routes.matches(101, 2));
  assert(routes.matches(101, 3) && routes.matches(202, 3));
  assert(!routes.matches(0, 3) && !routes.matches(303, 3));
  assert(!routes.matches(101, 0) && !routes.matches(101, 4));

  assert(!routes.assign(0, 202));
  assert(routes.targets[0] == 101 && routes.targets[1] == 202);
  assert(routes.assign(0, 303));
  assert(!routes.matches(101, 3) && routes.matches(303, 1));
  routes.forget(101);  // A retired old incarnation cannot clear its replacement.
  routes.forget(999);
  routes.forget(0);
  assert(routes.targets[0] == 303 && routes.targets[1] == 202);
  routes.forget(303);
  assert(routes.active_mask(true, true, true) == 2);
  assert(!routes.matches(303, 3) && routes.matches(202, 2));
  assert(routes.assign(0, std::numeric_limits<std::uint64_t>::max()));
  assert(routes.matches(std::numeric_limits<std::uint64_t>::max(), 1));
  routes.forget(202);
  assert(routes.active_mask(true, true, true) == 1);
  routes.forget(std::numeric_limits<std::uint64_t>::max());
  assert(routes.active_mask(true, true, true) == 0);
  assert(routes.assign(1, 303));  // A forgotten ID may be explicitly assigned again.
  assert(routes.active_mask(true, true, true) == 2);

  taxi_camera::TaxiButtonRoutes replacements;
  assert(!replacements.adopt_detected({0, 1}));
  assert(!replacements.adopt_detected({1, 1}));
  assert(replacements.adopt_detected({11569, 11170}));
  replacements.forget(11170);
  assert(replacements.adopt_detected({43712, 11569}));  // New right ID is larger than surviving left.
  assert((replacements.targets == std::array<std::uint64_t, 2>{11569, 43712}));
  assert(replacements.matches(11569, 1) && replacements.matches(43712, 2));
  assert(replacements.adopt_detected({43712, 11569}));  // Reordered detections never swap live sides.
  assert(replacements.targets[0] == 11569);
  replacements.forget(11569);
  assert(!replacements.adopt_detected({60000, 50000}));  // A pair without the survivor cannot replace it.
  assert(replacements.targets[0] == 0 && replacements.targets[1] == 43712);
  assert(replacements.adopt_detected({60000, 43712}));  // Symmetric left-only replacement.
  assert(replacements.targets[0] == 60000 && replacements.targets[1] == 43712);
  replacements.forget(60000);
  replacements.forget(43712);
  assert(!replacements.adopt_detected({80000, 70000}));       // Both lost: no new creation-order guess.
  assert(replacements.adopt_detected({70000, 80000}, true));  // Exact names restore semantic identity.
  assert(replacements.targets[0] == 70000 && replacements.targets[1] == 80000);
  replacements.forget(70000);
  replacements.forget(80000);
  assert(replacements.assign(1, 90000));
  assert(replacements.adopt_detected({100000, 90000}));  // Explicit manual side remains authoritative.
  assert(replacements.targets[1] == 90000);

  std::puts("Taxi button routes: PASS");
}
