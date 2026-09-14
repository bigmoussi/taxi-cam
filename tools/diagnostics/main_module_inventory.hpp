#pragma once

#include <windows.h>

#include "../../src/camera/activation_mask.hpp"
#include "../../src/camera/code_contract.hpp"
#include "../../src/camera/source_view.hpp"
#include "../../src/camera/aircraft_inventory.hpp"
#include "callee_prefix.hpp"
#include "command_list_inventory.hpp"
#include "guard_inventory.hpp"
#include "../../src/camera/image_inventory.hpp"
#include "import_slot.hpp"
#include "pointer_inventory.hpp"
#include "reference_inventory.hpp"
#include "rtti_metadata.hpp"
#include "service_inventory.hpp"
#include "slot_prefix.hpp"
#include "static_literals.hpp"
#include "static_numeric.hpp"

namespace taxi_camera::discovery {

enum class CameraContextKind { none, creation, methods, view_setup, view_state, service, leaves, aircraft, command_list };

struct MainModuleInventory {
  bool main_module_verified = false;
  // Basename only; never includes the installation/user directory or base address.
  std::wstring module_name;
  std::wstring candidate_module_name;
  std::string identity_method = "unverified";
  std::uint32_t process_id = 0;
  std::uint32_t windows_error = 0;
  std::uint32_t mapped_image_size = 0;
  std::uint32_t header_region_state = 0;
  std::uint32_t header_region_type = 0;
  std::uint32_t header_region_protection = 0;
  std::uint32_t header_query_error = 0;
  std::uint32_t last_memory_read_error = 0;
  bool header_allocation_matches = false;
  bool memory_read_attempted = false;
  bool references_requested = false;
  bool callers_requested = false;
  bool functions_requested = false;
  CameraContextKind camera_context_kind = CameraContextKind::none;
  bool llvm_decoder_ready = false;
  ReferenceInventory references;
  FunctionInventory functions;
  PointerInventory manager_pointer_table;
  GuardInventory guard_metadata;
  StaticLiteralInventory setup_literals;
  ServiceInventory renderer_service;
  CommandListInventory command_list_type;
  CalleePrefixInventory callee_prefixes;
  BranchPrefixInventory branch_prefixes;
  BranchPrefixInventory component_branch_prefixes;
  AircraftInventory aircraft;
  SlotPrefixInventory aircraft_method_prefix;
  AircraftInventory aircraft_controller;
  SlotPrefixInventory aircraft_controller_method_prefix;
  AircraftInventory aircraft_selected_object;
  SlotPrefixInventory aircraft_selected_object_method_prefix;
  AircraftInventory aircraft_component;
  AircraftInventory camera_keys;
  RttiMetadata component_rtti;
  ImportSlotInventory cast_import;
  StaticNumericInventory pose_constants;
  native_camera::CodeContractInventory code_contract;
  native_camera::ActivationMaskInventory activation_disable_mask;
  native_camera::SourceViewSnapshot source_pose;
  bool manager_slot_protection_valid = false;
  std::uint32_t manager_slot_protection = 0;
  std::uint32_t manager_slot_query_error = 0;
  Inventory image;
};

// This synchronous operation must be explicitly requested by its caller.
// Only GetModuleHandleW(nullptr)'s exact handle is accepted, not a DLL, file,
// another process, arbitrary allocation, or a null/default argument.
// No result is logged or written to disk by this helper.
MainModuleInventory inspect_main_module(HMODULE explicitly_selected_main, Limits limits = {});

// Opens exactly this PID with query/read rights, verifies the same user SID and
// FlightSimulator2024.exe main-image identity. Reads stay in that mapped image
// except the explicitly bounded service-vptr and active-aircraft field modes.
// Never enables debugger privileges, opens other processes, or modifies memory.
MainModuleInventory inspect_simulator_main_module(std::uint32_t explicitly_selected_pid,
                                                  Limits limits = {},
                                                  bool include_references = false,
                                                  CameraContextKind camera_context = CameraContextKind::none,
                                                  bool include_callers = false);

}  // namespace taxi_camera::discovery
