// No engine addresses or calls. Compile-only wrappers compare the two Windows
// x64 compiler targets for the EXACT primitive/pointer signatures used by probe.
// Matching assembly establishes only agreement on these declarations, not that
// the declarations describe any particular private simulator routine.

using U64 = unsigned long long;
static_assert(sizeof(U64) == 8);

extern "C" {

void call_abi_initialize(void (*callee)(void*), void* descriptor) {
  callee(descriptor);
}

U64 call_abi_create(U64 (*callee)(void*, const void*), void* manager, const void* descriptor) {
  return callee(manager, descriptor);
}

void call_abi_erase(void (*callee)(void*, U64), void* manager, U64 id) {
  callee(manager, id);
}

void call_abi_activate(void (*callee)(void*, U64, bool), void* manager, U64 id, bool enabled) {
  callee(manager, id, enabled);
}

void call_abi_activate_true(void (*callee)(void*, U64, bool), void* manager, U64 id) {
  callee(manager, id, true);
}

void call_abi_vector(void (*callee)(void*, const double*), void* object, const double* vector) {
  callee(object, vector);
}

void call_abi_scalar(void (*callee)(void*, float), void* camera, float fov) {
  callee(camera, fov);
}

double* call_abi_position(double* (*callee)(void*, double*), void* source, double* output) {
  return callee(source, output);
}

bool call_abi_position_checked(double* (*callee)(void*, double*), void* source, double* output) {
  return callee(source, output) == output;
}

float* call_abi_orientation(float* (*callee)(void*, float*), void* source, float* output) {
  return callee(source, output);
}

double* call_abi_quaternion_rotate(double* (*callee)(const float*, double*, const double*),
                                   const float* quaternion,
                                   double* output,
                                   const double* axis) {
  return callee(quaternion, output, axis);
}

void call_abi_view_update(void (*callee)(void*), void* view) {
  callee(view);
}

void* call_abi_view_output(void* (*callee)(void*), void* view) {
  return callee(view);
}

bool call_abi_view_output_checked(void* (*callee)(void*), void* view) {
  return callee(view) == static_cast<char*>(view) + 144;
}

}  // extern "C"

// Deliberately incompatible control: MSVC and MinGW disagree on this aggregate
// return. The gate must observe a difference to avoid a vacuous target check.
struct ConstructedPair {
  float x;
  float y;
  ConstructedPair();
};

extern "C" void call_abi_incompatible_return(ConstructedPair (*callee)(), ConstructedPair* output) {
  *output = callee();
}
