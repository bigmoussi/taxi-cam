#include "body_pose_math.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void check(bool okay) {
  ++checks;
  if (!okay) {
    std::fprintf(stderr, "body math check%u failed\n", checks);
    std::exit(1);
  }
}
bool near(double a, double b, double e = 1e-8) {
  return std::abs(a - b) <= e;
}
int main() {
  using namespace body_math;
  auto zero = body({}, 0);
  check(valid_body_pose(zero));
  check(near(zero.origin[0], 6378137));
  check(zero.right == Vector3{0, 1, 0});
  check(zero.up == Vector3{1, 0, 0});
  check(zero.forward == Vector3{0, 0, 1});
  Telemetry p{};
  p.heading = 90;
  auto east = body(p, 0);
  check(near(east.forward[1], 1));
  check(near(east.right[2], -1));
  p.heading = 0;
  p.pitch = 30;
  auto down = body(p, 0);
  check(near(down.forward[0], -0.5));
  check(near(down.forward[2], std::sqrt(0.75)));
  p.pitch = 0;
  p.bank = 30;
  auto left = body(p, 0);
  check(near(left.right[0], 0.5));
  check(near(left.up[1], -0.5));
  for (double lat : {-80., -45., 0., 47.445, 80.})
    for (double lon : {-170., -70., 0., 19.258, 170.})
      for (double h : {-200., 0., 200., 10000.}) {
        Vector3 result{};
        check(geodetic(ecef(lat, lon, h), result));
        check(near(result[0], lat));
        check(near(result[1], lon));
        check(near(result[2], h, 1e-5));
        Telemetry v{lat, lon, h, 25, -30, 132};
        check(valid_body_pose(body(v, 43.2)));
      }
  Vector3 result{};
  check(!geodetic({}, result));
  check(!geodetic({std::numeric_limits<double>::quiet_NaN(), 0, 0}, result));
  p.latitude = 91;
  check(!valid(p));
  p = {};
  p.heading = std::numeric_limits<double>::infinity();
  check(!valid(p));
  const Vector3 private_camera{4079681.4634014531, 1425339.8962134011, 4675513.2694373801};
  check(geodetic(private_camera, result));
  check(near(result[0], 47.44505121780648, 2e-9));
  check(near(result[1], 19.258152952902243, 2e-9));
  const double correction = result[2] - 153.6362862912938;
  check(near(correction, 43.21202333, 1e-5));
  const Telemetry live{47.445242546783085,  19.257834308799911,   150.90905630500379,
                       0.75300593274972782, -0.04080524797177993, 132.44517097583434};
  const auto live_body = body(live, correction);
  check(valid_body_pose(live_body));
  Vector3 offset{};
  for (unsigned i = 0; i < 3; ++i)
    offset[i] = private_camera[i] - live_body.origin[i];
  const Vector3 local{mount_detail::dot(offset, live_body.right), mount_detail::dot(offset, live_body.up),
                      mount_detail::dot(offset, live_body.forward)};
  // Independently requested CameraGet AIRCRAFT gives left/up/forward. Its
  // lateral sign is opposite semantic body right; agreement here validates
  // the world/body mapping against the live pair, not a hardcoded pilot mount.
  check(near(local[0], -0.5244074940876278, 0.01));
  check(near(local[1], 3.147558034473149, 0.01));
  check(near(local[2], 32.05282137538828, 0.01));
  std::printf("{\"passed\":true,\"checks\":%u,\"live_local_right_up_forward\":[%.9f,%.9f,%.9f],\"vertical_correction_m\":%.9f}\n", checks,
              local[0], local[1], local[2], correction);
}
