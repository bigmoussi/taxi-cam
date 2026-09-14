#include "../src/scene_capture_manager.hpp"
#include <cstdio>
int main() {
  using M = taxi_camera::SceneCaptureManager;
  using L = M::List;
  std::printf("{\"lists\":%zu,\"stride\":%zu,\"native\":%zu,\"generation\":%zu,\"capacity\":%zu}\n",
    offsetof(M,lists_),sizeof(L),offsetof(L,native),offsetof(L,object_generation),M::MaximumLists);
}
