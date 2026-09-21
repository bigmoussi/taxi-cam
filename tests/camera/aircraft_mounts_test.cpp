#include "../../src/camera/aircraft_mounts.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void require(bool condition, const char* label) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
  }
}
void close(double actual, double expected, const char* label, double tolerance = 1e-8) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, label);
}
void vector_close(const Vector3& actual, const Vector3& expected, const char* label, double tolerance = 1e-8) {
  for (unsigned i = 0; i < 3; ++i)
    close(actual[i], expected[i], label, tolerance);
}
void empty(const MountedPose& pose) {
  vector_close(pose.position, {}, "failure clears position");
  vector_close(pose.target, {}, "failure clears target");
  vector_close(pose.up, {}, "failure clears up");
  require(pose.fov == 0, "failure clears FOV");
}

void identity_and_angles() {
  BodyPose body;
  MountConfig config{{2, 3, 4}, 0, 0, 0.9f};
  MountedPose pose;
  require(make_mounted_pose(body, config, pose), "identity mount");
  vector_close(pose.position, {2, 3, 4}, "identity position");
  vector_close(pose.target, {2, 3, 5}, "zero angle forward");
  vector_close(pose.up, {0, 1, 0}, "zero angle up");
  require(pose.fov == config.fov_radians, "FOV unchanged");
  config.yaw_degrees = 90;
  require(make_mounted_pose(body, config, pose), "right angle yaw");
  vector_close(pose.target, {3, 3, 4}, "positive yaw looks right");
  config.yaw_degrees = 0;
  config.pitch_degrees = 30;
  require(make_mounted_pose(body, config, pose), "positive pitch");
  vector_close(pose.target, {2, 3.5, 4 + std::sqrt(0.75)}, "positive pitch looks up");
  vector_close(pose.up, {0, std::sqrt(0.75), -0.5}, "pitch rotates up too");
}

void defaults() {
  const auto mounts = default_mounts();
  require(valid_mounts(mounts), "valid defaults");
  std::array<MountedPose, kMaxCameraFeeds> poses;
  require(make_mounted_pair({}, mounts, poses), "default pair");
  vector_close(poses[0].position, {0, -1.75, 26.950668984}, "nose position retained from verified live framing");
  vector_close(poses[1].position, {0, 18, -25}, "tail framing moves one metre aft");
  require(mounts[0].pitch_degrees == -17.5 && mounts[1].pitch_degrees == -32, "reference framing pitches");
  close(mounts[0].fov_radians, 1.24, "nose modest widening from live-tuned lens", 1e-7);
  close(mounts[1].fov_radians, 1.02, "tail modest widening from live-tuned lens", 1e-7);
  const std::array<Vector3, 2> aims{{{0, -2.05070579950427, 27.9043859347482}, {0, 17.4700807357668, -24.1519519038436}}};
  for (unsigned n = 0; n < 2; ++n) {
    Vector3 expected{}, direction{};
    for (unsigned i = 0; i < 3; ++i) {
      expected[i] = aims[n][i] - poses[n].position[i];
      direction[i] = poses[n].target[i] - poses[n].position[i];
    }
    const double length = std::sqrt(mount_detail::dot(expected, expected));
    for (double& component : expected)
      component /= length;
    vector_close(direction, expected, "default aiming landmark");
    close(mount_detail::dot(direction, poses[n].up), 0, "orthogonal aim and up");
  }
  require(poses[0].position != poses[1].position && poses[0].target != poses[1].target, "distinct two cameras");
}

void reference_framing() {
  const auto mounts = default_mounts();
  std::array<MountedPose, kMaxCameraFeeds> poses;
  require(make_mounted_pair({}, mounts, poses), "reference framing pair");
  constexpr double radians = 3.14159265358979323846 / 180.0;
  constexpr Vector3 wheel{0, -4.596384, 30.22092};
  const double wheel_depression = std::atan2(wheel[1] - poses[0].position[1], wheel[2] - poses[0].position[2]);
  // This is a configured contact landmark, not a measured tyre-center pixel.
  // Keep the proven mount position and test its angle relative to the new aim.
  require(wheel_depression > -42 * radians && wheel_depression < -40 * radians, "unchanged wheel-landmark bearing");
  const double relative_wheel_angle = wheel_depression - mounts[0].pitch_degrees * radians;
  require(relative_wheel_angle < 0, "wheel is below pane center to retain strut");
  require(relative_wheel_angle > -mounts[0].fov_radians * 0.5, "wheel within configured angular framing");

  Vector3 tail_forward{};
  for (unsigned i = 0; i < 3; ++i)
    tail_forward[i] = poses[1].target[i] - poses[1].position[i];
  // Config point.14 and point.15/16 are collision landmarks, not mesh bounds.
  // Keep those tail tips outside the lens framing instead of a chase view.
  for (const Vector3 landmark :
       {Vector3{0, 20.2681813584, -37.572696}, Vector3{-15.02664, 5.343144, -37.530024}, Vector3{15.02664, 5.343144, -37.530024}}) {
    Vector3 relative{};
    for (unsigned i = 0; i < 3; ++i)
      relative[i] = landmark[i] - poses[1].position[i];
    const double depth = mount_detail::dot(relative, tail_forward);
    require(depth <= 0 || std::abs(mount_detail::dot(relative, poses[1].up)) > depth * std::tan(mounts[1].fov_radians * 0.5),
            "tail tip landmark behind lens or outside vertical framing");
  }
  require(mounts[1].pitch_degrees * radians + mounts[1].fov_radians * 0.5 < 0, "level-aircraft tail frame excludes horizon");
  // Both wing-root leading-edge landmarks remain ahead of the lens.
  for (const double side : {-1.0, 1.0}) {
    const Vector3 relative{side * 5.139168, 0.292608 - poses[1].position[1], 15.822168 - poses[1].position[2]};
    require(mount_detail::dot(relative, tail_forward) > 0, "wing root ahead of lens");
  }
}

void follows_body() {
  // A 90-degree body heading rotates right to -Z and forward to +X.
  BodyPose body{{4e6, 1.4e6, 4.6e6}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
  const auto mounts = default_mounts();
  std::array<MountedPose, kMaxCameraFeeds> local{}, world{}, moved{};
  require(make_mounted_pair({}, mounts, local) && make_mounted_pair(body, mounts, world), "world body transform");
  for (unsigned n = 0; n < 2; ++n) {
    vector_close(world[n].position,
                 {body.origin[0] + local[n].position[2], body.origin[1] + local[n].position[1], body.origin[2] - local[n].position[0]},
                 "mount rotates with body", 1e-7);
    vector_close(world[n].up, {local[n].up[2], local[n].up[1], -local[n].up[0]}, "up rotates with body");
  }
  body.origin[0] += 10;
  body.origin[1] -= 2;
  require(make_mounted_pair(body, mounts, moved), "moving aircraft");
  for (unsigned n = 0; n < 2; ++n)
    for (unsigned i = 0; i < 3; ++i) {
      const double displacement = i == 0 ? 10 : i == 1 ? -2 : 0;
      close(moved[n].position[i] - world[n].position[i], displacement, "position follows translation", 1e-7);
      close(moved[n].target[i] - world[n].target[i], displacement, "aim follows translation", 1e-7);
    }
  // Roll changes the body up vector, not just the heading.
  body = {{}, {0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
  MountedPose pose;
  require(make_mounted_pose(body, MountConfig{{2, 3, 4}}, pose), "banked body");
  vector_close(pose.position, {-3, 2, 4}, "banked mount offset");
  vector_close(pose.up, {-1, 0, 0}, "banked mount up");
  // Semantic aircraft right/up/forward has opposite handedness to conventional
  // ECEF. At latitude0/longitude0/heading north: right=+Y, up=+X, forward=+Z.
  body = {{6378137, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}};
  require(make_mounted_pose(body, MountConfig{{2, 3, 4}}, pose), "opposite handedness ECEF body basis");
  vector_close(pose.position, {6378140, 2, 4}, "semantic body axes in ECEF");
  vector_close(pose.target, {6378140, 2, 5}, "northward aim in ECEF");
  vector_close(pose.up, {1, 0, 0}, "outward up in ECEF");
}

void refusals() {
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  const auto defaults_pair = default_mounts();
  MountedPose pose;
  for (double bad : {nan, inf, -inf}) {
    for (unsigned field = 0; field < 4; ++field)
      for (unsigned coordinate = 0; coordinate < 3; ++coordinate) {
        BodyPose body;
        std::array<Vector3*, 4> fields{&body.origin, &body.right, &body.up, &body.forward};
        (*fields[field])[coordinate] = bad;
        require(!make_mounted_pose(body, defaults_pair[0], pose), "nonfinite body rejected");
        empty(pose);
      }
    for (unsigned coordinate = 0; coordinate < 3; ++coordinate) {
      auto config = defaults_pair[0];
      config.position_m[coordinate] = bad;
      require(!make_mounted_pose({}, config, pose), "nonfinite mount rejected");
      empty(pose);
    }
    auto config = defaults_pair[0];
    config.pitch_degrees = bad;
    require(!valid_mount(config), "nonfinite pitch rejected");
    config = defaults_pair[0];
    config.yaw_degrees = bad;
    require(!valid_mount(config), "nonfinite yaw rejected");
    config = defaults_pair[0];
    config.fov_radians = static_cast<float>(bad);
    require(!valid_mount(config), "nonfinite FOV rejected");
  }
  for (Vector3 bad_forward : {Vector3{0, 0, 2}, Vector3{0, 1, 0}, Vector3{}}) {
    BodyPose body;
    body.forward = bad_forward;
    require(!make_mounted_pose(body, defaults_pair[0], pose), "scale shear or zero basis refused");
    empty(pose);
  }
  for (float bad_fov : {0.0f, -1.0f, kMountMinimumFov - 0.001f, kMountMaximumFov + 0.001f}) {
    auto mounts = defaults_pair;
    mounts[1].fov_radians = bad_fov;
    std::array<MountedPose, kMaxCameraFeeds> pair;
    require(make_mounted_pair({}, defaults_pair, pair), "seed pair before failure");
    require(!make_mounted_pair({}, mounts, pair), "bad second mount rejects complete pair");
    for (const auto& item : pair)
      empty(item);
  }
  auto config = defaults_pair[0];
  for (double sign : {-1.0, 1.0}) {
    config = {{sign * 500, sign * 500, sign * 500}, sign * 89, sign * 180, kMountMinimumFov};
    require(make_mounted_pose({}, config, pose), "inclusive position and angle limits");
    config.fov_radians = kMountMaximumFov;
    require(make_mounted_pose({}, config, pose), "inclusive maximum FOV");
    config.position_m[2] = sign * 500.001;
    require(!valid_mount(config), "offset limit");
    config = defaults_pair[0];
    config.pitch_degrees = sign * 89.001;
    require(!valid_mount(config), "pitch limit");
    config = defaults_pair[0];
    config.yaw_degrees = sign * 180.001;
    require(!valid_mount(config), "yaw limit");
  }
  BodyPose enormous;
  enormous.origin = {1e30, 1e30, 1e30};
  require(!make_mounted_pose(enormous, defaults_pair[0], pose), "unrepresentable look-at precision refused");
  empty(pose);
}
}  // namespace

int main() {
  identity_and_angles();
  defaults();
  reference_framing();
  follows_body();
  refusals();
  std::printf("PASS aircraft mounts: %u checks\n", checks);
}
