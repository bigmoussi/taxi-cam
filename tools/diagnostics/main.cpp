#include "llvm_decoder.hpp"
#include "main_module_inventory.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
using namespace taxi_camera::discovery;

std::string utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const auto size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return "<invalid module name>";
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

void json_string(const std::string& value) {
  std::cout << '"';
  for (const auto c : value) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      std::cout << '\\' << c;
    } else if (byte < 32) {
      char escaped[7];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      std::cout << escaped;
    } else {
      std::cout << c;
    }
  }
  std::cout << '"';
}

void report_pointer_entry(const PointerEntry& entry) {
  std::cout << "{\"index\": " << entry.index << ", \"source_rva\": " << entry.source_rva << ", \"target_rva\": " << entry.target_rva
            << ", \"category\": ";
  json_string(pointer_category_name(entry.category));
  std::cout << '}';
}

const char* source_view_status(taxi_camera::native_camera::SourceViewStatus status) {
  using taxi_camera::native_camera::SourceViewStatus;
  switch (status) {
    case SourceViewStatus::not_inspected:
      return "not_inspected";
    case SourceViewStatus::ready:
      return "ready";
    case SourceViewStatus::no_source:
      return "no_source";
    case SourceViewStatus::invalid_pool:
      return "invalid_pool";
    case SourceViewStatus::invalid_pointer:
      return "invalid_pointer";
    case SourceViewStatus::pool_changed:
      return "pool_changed";
    case SourceViewStatus::read_failed:
      return "read_failed";
    case SourceViewStatus::changed:
      return "changed";
    case SourceViewStatus::read_limit:
      return "read_limit";
  }
  return "unknown";
}

template <typename PrefixInventory>
void report_prefixes(const char* label, const PrefixInventory& prefixes) {
  std::cout << ",\n  ";
  json_string(label);
  std::cout << ": {\"valid\": " << prefixes.valid << ", \"error\": ";
  json_string(prefixes.error);
  std::cout << ", \"read_bytes\": " << prefixes.read_bytes << ", \"proof_bytes\": " << prefixes.proof_bytes
            << ", \"read_failures\": " << prefixes.read_failures << ", \"entries\": [";
  bool first_prefix = true;
  for (const auto& prefix : prefixes.entries) {
    if (!first_prefix)
      std::cout << ',';
    first_prefix = false;
    if constexpr (requires { prefix.proof_call_rva; })
      std::cout << "{\"proof_call_rva\": " << prefix.proof_call_rva;
    else if constexpr (requires { prefix.proof_branch_rva; })
      std::cout << "{\"proof_branch_rva\": " << prefix.proof_branch_rva;
    else
      std::cout << "{\"proof_slot_rva\": " << prefix.proof_slot_rva;
    std::cout << ", \"entry_rva\": " << prefix.entry_rva << ", \"bytes_read\": " << prefix.bytes_read
              << ", \"decoded_bytes\": " << prefix.decoded_bytes << ", \"ret_observed\": " << prefix.ret_observed << ", \"stop_reason\": ";
    json_string(prefix.stop_reason);
    std::cout << ", \"error\": ";
    json_string(prefix.error);
    std::cout << ", \"instructions\": [";
    bool first_instruction = true;
    for (const auto& instruction : prefix.instructions) {
      if (!first_instruction)
        std::cout << ',';
      first_instruction = false;
      std::cout << "{\"rva\": " << instruction.rva << ", \"text\": ";
      json_string(instruction.text);
      std::cout << ", \"direct_call_target_rva\": " << instruction.direct_call_target_rva << '}';
    }
    std::cout << "]}";
  }
  std::cout << "]}";
}

void report(const MainModuleInventory& result, std::int64_t elapsed_ms) {
  const auto& image = result.image;
  std::cout << std::boolalpha << "{\n  \"schema\": 1,\n  \"process_id\": " << result.process_id << ",\n  \"module_name\": ";
  json_string(utf8(result.module_name));
  std::cout << ",\n  \"candidate_module_name\": ";
  json_string(utf8(result.candidate_module_name));
  std::cout << ",\n  \"identity_method\": ";
  json_string(result.identity_method);
  std::cout << ",\n  \"main_module_verified\": " << result.main_module_verified << ",\n  \"valid_image\": " << image.valid_image
            << ",\n  \"error\": ";
  json_string(image.error);
  std::cout << ",\n  \"windows_error\": " << result.windows_error << ",\n  \"elapsed_ms\": " << elapsed_ms
            << ",\n  \"mapped_image_size\": " << result.mapped_image_size << ",\n  \"header_region_state\": " << result.header_region_state
            << ",\n  \"header_region_type\": " << result.header_region_type
            << ",\n  \"header_region_protection\": " << result.header_region_protection
            << ",\n  \"header_allocation_matches\": " << result.header_allocation_matches
            << ",\n  \"header_query_error\": " << result.header_query_error
            << ",\n  \"memory_read_attempted\": " << result.memory_read_attempted
            << ",\n  \"last_memory_read_error\": " << result.last_memory_read_error << ",\n  \"machine\": " << image.machine
            << ",\n  \"timestamp_raw\": " << image.timestamp << ",\n  \"image_size\": " << image.image_size
            << ",\n  \"checksum\": " << image.checksum << ",\n  \"section_count\": " << image.section_count
            << ",\n  \"scanned_bytes\": " << image.scanned_bytes << ",\n  \"metadata_bytes\": " << image.metadata_bytes
            << ",\n  \"skipped_bytes\": " << image.skipped_bytes << ",\n  \"read_failures\": " << image.read_failures
            << ",\n  \"malformed_exports\": " << image.malformed_exports << ",\n  \"scan_truncated\": " << image.scan_truncated
            << ",\n  \"exports_truncated\": " << image.exports_truncated << ",\n  \"record_limit_reached\": " << image.record_limit_reached
            << ",\n  \"records\": [";
  bool first = true;
  for (const auto& record : image.records) {
    if (!first) {
      std::cout << ',';
    }
    first = false;
    std::cout << "\n    {\"kind\": ";
    json_string(record.kind == RecordKind::export_name        ? "export_name"
                : record.kind == RecordKind::forwarded_export ? "forwarded_export"
                                                              : "ascii_literal");
    std::cout << ", \"rva\": " << record.rva << ", \"function_rva\": " << record.function_rva << ", \"ordinal\": " << record.ordinal
              << ", \"section\": ";
    json_string(record.section);
    std::cout << ", \"keyword\": ";
    json_string(record.keyword);
    std::cout << ", \"text\": ";
    json_string(record.text);
    std::cout << '}';
  }
  std::cout << "\n  ],\n  \"references_requested\": " << result.references_requested;
  std::cout << ",\n  \"callers_requested\": " << result.callers_requested;
  if (result.references_requested) {
    const auto& references = result.references;
    std::cout << ",\n  \"reference_scan\": {\n    \"llvm_decoder_ready\": " << result.llvm_decoder_ready
              << ",\n    \"valid_targets\": " << references.valid_targets << ",\n    \"error\": ";
    json_string(references.error);
    std::cout << ",\n    \"code_bytes\": " << references.code_bytes << ",\n    \"decoded_bytes\": " << references.decoded_bytes
              << ",\n    \"candidate_count\": " << references.candidate_count
              << ",\n    \"runtime_function_count\": " << references.runtime_function_count
              << ",\n    \"missing_function_bounds\": " << references.missing_function_bounds
              << ",\n    \"failed_boundary_checks\": " << references.failed_boundary_checks
              << ",\n    \"read_failures\": " << references.read_failures
              << ",\n    \"code_limit_reached\": " << references.code_limit_reached
              << ",\n    \"decode_limit_reached\": " << references.decode_limit_reached
              << ",\n    \"reference_limit_reached\": " << references.reference_limit_reached << ",\n    \"references\": [";
    bool first_reference = true;
    for (const auto& reference : references.references) {
      if (!first_reference) {
        std::cout << ',';
      }
      first_reference = false;
      std::cout << "\n      {\"target_label\": ";
      json_string(reference.target_label);
      std::cout << ", \"target_rva\": " << reference.target_rva << ", \"function_begin_rva\": " << reference.function_begin_rva
                << ", \"function_end_rva\": " << reference.function_end_rva << ", \"instruction_rva\": " << reference.instruction_rva
                << ", \"context\": [";
      bool first_instruction = true;
      for (const auto& instruction : reference.context) {
        if (!first_instruction) {
          std::cout << ',';
        }
        first_instruction = false;
        std::cout << "{\"rva\": " << instruction.rva << ", \"text\": ";
        json_string(instruction.text);
        std::cout << ", \"direct_call_target_rva\": " << instruction.direct_call_target_rva << '}';
      }
      std::cout << "]}";
    }
    std::cout << "\n    ]\n  }";
  }
  std::cout << ",\n  \"functions_requested\": " << result.functions_requested;
  std::cout << ",\n  \"camera_context_kind\": ";
  json_string(result.camera_context_kind == CameraContextKind::command_list ? "command_list"
              : result.camera_context_kind == CameraContextKind::aircraft   ? "aircraft"
              : result.camera_context_kind == CameraContextKind::leaves     ? "leaves"
              : result.camera_context_kind == CameraContextKind::service    ? "service"
              : result.camera_context_kind == CameraContextKind::view_state ? "view_state"
              : result.camera_context_kind == CameraContextKind::view_setup ? "view_setup"
              : result.camera_context_kind == CameraContextKind::methods    ? "methods"
              : result.camera_context_kind == CameraContextKind::creation   ? "creation"
                                                                            : "none");
  if (result.camera_context_kind == CameraContextKind::view_setup || result.camera_context_kind == CameraContextKind::view_state) {
    const auto& literals = result.setup_literals;
    std::cout << ",\n  \"setup_literals\": {\"valid\": " << literals.valid << ", \"error\": ";
    json_string(literals.error);
    std::cout << ", \"read_bytes\": " << literals.read_bytes << ", \"read_failures\": " << literals.read_failures << ", \"records\": [";
    bool first_literal = true;
    for (const auto& literal : literals.records) {
      if (!first_literal)
        std::cout << ',';
      first_literal = false;
      std::cout << "{\"rva\": " << literal.rva << ", \"bytes\": " << literal.bytes << ", \"text\": ";
      json_string(literal.text);
      std::cout << ", \"error\": ";
      json_string(literal.error);
      std::cout << '}';
    }
    std::cout << "]}";
  }
  if (result.camera_context_kind == CameraContextKind::service) {
    const auto& service = result.renderer_service;
    std::cout << ",\n  \"renderer_service\": {\"valid\": " << service.valid << ", \"error\": ";
    json_string(service.error);
    std::cout << ", \"cached_present\": " << service.cached_present << ", \"stage\": ";
    json_string(service.stage);
    std::cout << ", \"image_bytes\": " << service.image_bytes << ", \"object_bytes\": " << service.object_bytes
              << ", \"read_failures\": " << service.read_failures << ", \"vtable_rva\": " << service.vtable_rva
              << ", \"method_slot_rva\": " << service.method_slot_rva << ", \"method_rva\": " << service.method_rva
              << ", \"cleanup_slot_rva\": " << service.cleanup_slot_rva << ", \"cleanup_method_rva\": " << service.cleanup_method_rva
              << '}';
  }
  if (result.camera_context_kind == CameraContextKind::command_list) {
    const auto& type = result.command_list_type;
    std::cout << ",\n  \"command_list_type\": {\n    \"valid\": " << type.valid << ",\n    \"available\": " << type.available
              << ",\n    \"stage\": ";
    json_string(type.stage);
    std::cout << ",\n    \"error\": ";
    json_string(type.error);
    std::cout << ",\n    \"type\": " << type.type << ",\n    \"type_name\": ";
    json_string(!type.available  ? "unavailable"
                : type.type == 0 ? "direct"
                : type.type == 1 ? "bundle"
                : type.type == 2 ? "compute"
                                 : "copy");
    std::cout << ",\n    \"vtable_rva\": " << type.vtable_rva << ",\n    \"image_bytes\": " << type.image_bytes
              << ",\n    \"object_bytes\": " << type.object_bytes << ",\n    \"read_failures\": " << type.read_failures << "\n  }";
  }
  if (result.camera_context_kind == CameraContextKind::aircraft) {
    const auto& aircraft = result.aircraft;
    std::cout << ",\n  \"aircraft\": {\"valid\": " << aircraft.valid << ", \"cached_present\": " << aircraft.cached_present
              << ", \"selected\": " << aircraft.selected << ", \"stage\": ";
    json_string(aircraft.stage);
    std::cout << ", \"error\": ";
    json_string(aircraft.error);
    std::cout << ", \"image_bytes\": " << aircraft.image_bytes << ", \"object_bytes\": " << aircraft.object_bytes
              << ", \"read_failures\": " << aircraft.read_failures << ", \"world_count\": " << aircraft.world_count
              << ", \"worlds_examined\": " << aircraft.worlds_examined << ", \"selected_world_index\": " << aircraft.selected_world_index
              << ", \"user_count\": " << aircraft.user_count << ", \"viewport_id\": " << aircraft.viewport_id
              << ", \"facade_vtable_rva\": " << aircraft.facade_vtable_rva << ", \"method_slot_rva\": " << aircraft.method_slot_rva
              << ", \"method_rva\": " << aircraft.method_rva << '}';
    report_prefixes("aircraft_method_prefix", result.aircraft_method_prefix);
    const auto& controller = result.aircraft_controller;
    std::cout << ",\n  \"aircraft_controller\": {\"valid\": " << controller.valid << ", \"object_present\": " << controller.object_present
              << ", \"stage\": ";
    json_string(controller.stage);
    std::cout << ", \"error\": ";
    json_string(controller.error);
    std::cout << ", \"image_bytes\": " << controller.image_bytes << ", \"object_bytes\": " << controller.object_bytes
              << ", \"read_failures\": " << controller.read_failures << ", \"object_vtable_rva\": " << controller.object_vtable_rva
              << ", \"object_method_slot_rva\": " << controller.object_method_slot_rva
              << ", \"object_method_rva\": " << controller.object_method_rva << '}';
    report_prefixes("aircraft_controller_method_prefix", result.aircraft_controller_method_prefix);
    const auto& selected = result.aircraft_selected_object;
    std::cout << ",\n  \"aircraft_selected_object\": {\"valid\": " << selected.valid << ", \"available\": " << selected.available
              << ", \"selected_object_present\": " << selected.selected_object_present << ", \"stage\": ";
    json_string(selected.stage);
    std::cout << ", \"error\": ";
    json_string(selected.error);
    std::cout << ", \"image_bytes\": " << selected.image_bytes << ", \"object_bytes\": " << selected.object_bytes
              << ", \"read_failures\": " << selected.read_failures << ", \"controller_validity\": " << selected.controller_validity
              << ", \"selected_object_index\": " << selected.selected_object_index
              << ", \"selected_object_vtable_rva\": " << selected.selected_object_vtable_rva
              << ", \"selected_object_method_slot_rva\": " << selected.selected_object_method_slot_rva
              << ", \"selected_object_method_rva\": " << selected.selected_object_method_rva << '}';
    report_prefixes("aircraft_selected_object_method_prefix", result.aircraft_selected_object_method_prefix);
    const auto& component = result.aircraft_component;
    std::cout << ",\n  \"aircraft_component\": {\"valid\": " << component.valid << ", \"available\": " << component.available
              << ", \"aircraft_present\": " << component.aircraft_present << ", \"component_present\": " << component.component_present
              << ", \"stage\": ";
    json_string(component.stage);
    std::cout << ", \"error\": ";
    json_string(component.error);
    std::cout << ", \"image_bytes\": " << component.image_bytes << ", \"object_bytes\": " << component.object_bytes
              << ", \"read_failures\": " << component.read_failures << ", \"component_count\": " << component.component_count
              << ", \"selected_component_index\": " << component.selected_component_index
              << ", \"aircraft_vtable_rva\": " << component.aircraft_vtable_rva
              << ", \"component_vtable_rva\": " << component.component_vtable_rva << '}';
    const auto& cast_import = result.cast_import;
    std::cout << ",\n  \"cast_import\": {\"valid\": " << cast_import.valid << ", \"available\": " << cast_import.available
              << ", \"status\": ";
    json_string(cast_import.status);
    std::cout << ", \"error\": ";
    json_string(cast_import.error);
    std::cout << ", \"module\": ";
    json_string(cast_import.module);
    std::cout << ", \"symbol\": ";
    json_string(cast_import.symbol);
    std::cout << ", \"iat_slot_rva\": " << cast_import.iat_slot_rva << ", \"lookup_thunk_rva\": " << cast_import.lookup_thunk_rva
              << ", \"read_bytes\": " << cast_import.read_bytes << ", \"read_failures\": " << cast_import.read_failures << '}';
    const auto& rtti = result.component_rtti;
    std::cout << ",\n  \"component_rtti\": {\"valid\": " << rtti.valid << ", \"stage\": ";
    json_string(rtti.stage);
    std::cout << ", \"error\": ";
    json_string(rtti.error);
    std::cout << ", \"read_bytes\": " << rtti.read_bytes << ", \"read_failures\": " << rtti.read_failures
              << ", \"vtable_rva\": " << rtti.vtable_rva << ", \"locator_rva\": " << rtti.locator_rva << ", \"offset\": " << rtti.offset
              << ", \"cd_offset\": " << rtti.cd_offset << ", \"type_rva\": " << rtti.type_rva
              << ", \"hierarchy_rva\": " << rtti.hierarchy_rva << ", \"hierarchy_attributes\": " << rtti.hierarchy_attributes
              << ", \"base_count\": " << rtti.base_count << ", \"source_type_present\": " << rtti.source_type_present
              << ", \"target_type_present\": " << rtti.target_type_present << ", \"bases\": [";
    for (std::size_t index = 0; index < rtti.bases.size(); ++index) {
      const auto& base = rtti.bases[index];
      if (index != 0)
        std::cout << ',';
      std::cout << "{\"index\": " << base.index << ", \"descriptor_rva\": " << base.descriptor_rva << ", \"type_rva\": " << base.type_rva
                << ", \"num_contained_bases\": " << base.num_contained_bases << ", \"mdisp\": " << base.mdisp
                << ", \"pdisp\": " << base.pdisp << ", \"vdisp\": " << base.vdisp << ", \"attributes\": " << base.attributes
                << ", \"hierarchy_rva\": " << base.hierarchy_rva << '}';
    }
    std::cout << "]}";
    const auto& keys = result.camera_keys;
    std::cout << ",\n  \"camera_keys\": {\"valid\": " << keys.valid << ", \"available\": " << keys.available
              << ", \"camera_keys_inspected\": " << keys.camera_keys_inspected << ", \"stage\": ";
    json_string(keys.stage);
    std::cout << ", \"error\": ";
    json_string(keys.error);
    std::cout << ", \"image_bytes\": " << keys.image_bytes << ", \"object_bytes\": " << keys.object_bytes
              << ", \"read_failures\": " << keys.read_failures << ", \"camera_key_count\": " << keys.camera_key_count
              << ", \"tail_matches\": " << keys.tail_matches << ", \"gear_matches\": " << keys.gear_matches
              << ", \"first_tail_match_index\": " << keys.first_tail_match_index
              << ", \"first_gear_match_index\": " << keys.first_gear_match_index << '}';
    const auto& pose = result.source_pose;
    std::cout << ",\n  \"source_pose\": {\"complete\": " << pose.complete << ", \"status\": ";
    json_string(source_view_status(pose.status));
    std::cout << ", \"error\": ";
    json_string(pose.error);
    std::cout << ", \"field\": ";
    json_string(pose.field);
    std::cout << ", \"rejected_alignment\": " << static_cast<unsigned>(pose.rejected_alignment) << ", \"read_bytes\": " << pose.read_bytes
              << ", \"read_failures\": " << pose.read_failures << ", \"candidates_examined\": " << pose.candidates_examined
              << ", \"fov\": " << std::setprecision(9) << pose.fov << '}';
    const auto& contract = result.code_contract;
    std::cout << ",\n  \"source_pose_code_contract\": {\"valid\": " << contract.valid << ", \"error\": ";
    json_string(contract.error);
    std::cout << ", \"read_bytes\": " << contract.read_bytes << ", \"metadata_bytes\": " << contract.metadata_bytes
              << ", \"read_failures\": " << contract.read_failures << ", \"relocations_checked\": " << contract.relocations_checked
              << ", \"range_count\": " << contract.records.size() << '}';
  }
  if (result.camera_context_kind == CameraContextKind::leaves || result.camera_context_kind == CameraContextKind::aircraft ||
      result.camera_context_kind == CameraContextKind::view_state) {
    report_prefixes("callee_prefixes", result.callee_prefixes);
    report_prefixes("branch_prefixes", result.branch_prefixes);
    if (result.camera_context_kind == CameraContextKind::aircraft)
      report_prefixes("component_branch_prefixes", result.component_branch_prefixes);
  }
  if (result.camera_context_kind == CameraContextKind::view_state) {
    std::cout << ",\n  \"image_sections\": [";
    for (std::size_t index = 0; index < image.sections.size(); ++index) {
      const auto& section = image.sections[index];
      if (index != 0)
        std::cout << ',';
      std::cout << "{\"rva\": " << section.rva << ", \"size\": " << section.size << ", \"flags\": " << section.flags << '}';
    }
    std::cout << ']';
    const auto& contract = result.code_contract;
    std::cout << ",\n  \"code_contract\": {\"valid\": " << contract.valid << ", \"error\": ";
    json_string(contract.error);
    std::cout << ", \"read_bytes\": " << contract.read_bytes << ", \"metadata_bytes\": " << contract.metadata_bytes
              << ", \"read_failures\": " << contract.read_failures << ", \"code_byte_limit\": 65536, \"metadata_byte_limit\": 1048576"
              << ", \"range_limit\": 32, \"range_byte_limit\": 8192, \"relocations_checked\": " << contract.relocations_checked
              << ", \"relocation_rva\": " << contract.relocation_rva << ", \"relocation_size\": " << contract.relocation_size
              << ", \"relocation_entries\": " << contract.relocation_entries << ", \"relocation_blocks\": " << contract.relocation_blocks
              << ", \"relocation_blocks_skipped\": " << contract.relocation_blocks_skipped << ", \"records\": [";
    for (std::size_t index = 0; index < contract.records.size(); ++index) {
      const auto& record = contract.records[index];
      if (index != 0)
        std::cout << ',';
      std::cout << "{\"rva\": " << record.rva << ", \"size\": " << record.size << ", \"hash\": \"0x" << std::hex << std::setfill('0')
                << std::setw(16) << record.hash << std::dec << std::setfill(' ') << "\"}";
    }
    std::cout << "]},\n  \"manager_slot_protection\": {\"rva\": 133571352, \"valid\": " << result.manager_slot_protection_valid
              << ", \"protection\": " << result.manager_slot_protection << ", \"query_error\": " << result.manager_slot_query_error << '}';
    const auto& mask = result.activation_disable_mask;
    std::cout << ",\n  \"activation_disable_mask\": {\"rva\": 130434096, \"valid\": " << mask.valid << ", \"error\": ";
    json_string(mask.error);
    std::cout << ", \"read_bytes\": " << mask.read_bytes << ", \"read_failures\": " << mask.read_failures << ", \"words\": [";
    for (std::size_t index = 0; index < mask.words.size(); ++index) {
      if (index != 0)
        std::cout << ',';
      std::cout << "\"0x" << std::hex << std::setfill('0') << std::setw(16) << mask.words[index] << std::dec << std::setfill(' ') << '"';
    }
    std::cout << "]}";
    const auto& numbers = result.pose_constants;
    std::cout << ",\n  \"pose_constants\": {\"valid\": " << numbers.valid << ", \"error\": ";
    json_string(numbers.error);
    std::cout << ", \"read_bytes\": " << numbers.read_bytes << ", \"read_failures\": " << numbers.read_failures << ", \"records\": [";
    for (std::size_t index = 0; index < numbers.records.size(); ++index) {
      const auto& record = numbers.records[index];
      if (index != 0)
        std::cout << ',';
      std::cout << "{\"rva\": " << record.rva << ", \"bytes\": " << record.bytes << ", \"error\": ";
      json_string(record.error);
      std::cout << ", \"values\": [";
      for (std::size_t scalar = 0; scalar < record.values.size(); ++scalar) {
        if (scalar != 0)
          std::cout << ',';
        std::cout << std::setprecision(17) << record.values[scalar];
      }
      std::cout << "]}";
    }
    std::cout << "]}";
  }
  if (result.functions_requested) {
    const auto& functions = result.functions;
    std::cout << ",\n  \"function_contexts\": {\n    \"llvm_decoder_ready\": " << result.llvm_decoder_ready
              << ",\n    \"valid_targets\": " << functions.valid_targets << ",\n    \"error\": ";
    json_string(functions.error);
    std::cout << ",\n    \"code_bytes\": " << functions.code_bytes << ",\n    \"decoded_bytes\": " << functions.decoded_bytes
              << ",\n    \"runtime_function_count\": " << functions.runtime_function_count
              << ",\n    \"read_failures\": " << functions.read_failures
              << ",\n    \"missing_function_bounds\": " << functions.missing_function_bounds << ",\n    \"partial\": " << functions.partial
              << ",\n    \"functions\": [";
    bool first_function = true;
    for (const auto& function : functions.functions) {
      if (!first_function)
        std::cout << ',';
      first_function = false;
      std::cout << "\n      {\"requested_rva\": " << function.requested_rva << ", \"function_begin_rva\": " << function.function_begin_rva
                << ", \"function_end_rva\": " << function.function_end_rva << ", \"bytes_read\": " << function.bytes_read
                << ", \"decoded_bytes\": " << function.decoded_bytes << ", \"truncated\": " << function.truncated
                << ", \"decode_failed\": " << function.decode_failed
                << ", \"requested_boundary_found\": " << function.requested_boundary_found << ", \"error\": ";
      json_string(function.error);
      std::cout << ", \"instructions\": [";
      bool first_instruction = true;
      for (const auto& instruction : function.instructions) {
        if (!first_instruction)
          std::cout << ',';
        first_instruction = false;
        std::cout << "{\"rva\": " << instruction.rva << ", \"text\": ";
        json_string(instruction.text);
        std::cout << ", \"direct_call_target_rva\": " << instruction.direct_call_target_rva << '}';
      }
      std::cout << "]}";
    }
    std::cout << "\n    ]\n  }";
    const auto& table = result.manager_pointer_table;
    std::cout << ",\n  \"manager_pointer_table\": {\n    \"valid_table\": " << table.valid_table << ",\n    \"error\": ";
    json_string(table.error);
    std::cout << ",\n    \"table_rva\": " << table.table_rva << ",\n    \"entry_limit\": " << table.entry_limit
              << ",\n    \"read_bytes\": " << table.read_bytes << ",\n    \"read_failures\": " << table.read_failures
              << ",\n    \"entry_limit_reached\": " << table.entry_limit_reached
              << ",\n    \"stopped_on_non_code\": " << table.stopped_on_non_code
              << ",\n    \"preceding_available\": " << table.preceding_available << ",\n    \"preceding\": ";
    report_pointer_entry(table.preceding);
    std::cout << ",\n    \"entries\": [";
    bool first_entry = true;
    for (const auto& entry : table.entries) {
      if (!first_entry)
        std::cout << ',';
      first_entry = false;
      report_pointer_entry(entry);
    }
    std::cout << "]\n  }";
    const auto& guard = result.guard_metadata;
    std::cout << ",\n  \"guard_metadata\": {\n    \"present\": " << guard.present << ",\n    \"valid\": " << guard.valid
              << ",\n    \"error\": ";
    json_string(guard.error);
    std::cout << ",\n    \"read_bytes\": " << guard.read_bytes << ",\n    \"read_failures\": " << guard.read_failures
              << ",\n    \"declared_size\": " << guard.declared_size << ",\n    \"guard_flags\": " << guard.guard_flags
              << ",\n    \"guard_cf_characteristic\": " << guard.guard_cf_characteristic
              << ",\n    \"cf_instrumented\": " << guard.cf_instrumented
              << ",\n    \"function_table_present\": " << guard.function_table_present
              << ",\n    \"dispatch_slot_present\": " << guard.dispatch_slot_present
              << ",\n    \"dispatch_slot_rva\": " << guard.dispatch_slot_rva << "\n  }";
  }
  std::cout << "\n}\n";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--decoder-self-test") {
    std::string error;
    if (!llvm_decoder_self_test(error)) {
      std::fprintf(stderr, "FAIL: %s\n", error.c_str());
      return 1;
    }
    std::puts("PASS: pinned LLVM C decoder parsed synthetic RIP-relative LEA and RET instructions.");
    return 0;
  }
  bool self = argc == 1;
  bool references = false;
  bool camera_callers = false;
  bool camera_context = false;
  bool camera_methods = false;
  bool camera_view_setup = false;
  bool camera_view_state = false;
  bool camera_service = false;
  bool camera_leaves = false;
  bool camera_aircraft = false;
  bool camera_command_list = false;
  std::uint32_t pid = 0;
  if (argc == 2 && std::string_view(argv[1]) == "--self") {
    self = true;
  } else if ((argc == 3 ||
              (argc == 4 && (std::string_view(argv[3]) == "--references" || std::string_view(argv[3]) == "--camera-context" ||
                             std::string_view(argv[3]) == "--camera-methods" || std::string_view(argv[3]) == "--camera-view-setup" ||
                             std::string_view(argv[3]) == "--camera-callers" || std::string_view(argv[3]) == "--camera-view-state" ||
                             std::string_view(argv[3]) == "--camera-service" || std::string_view(argv[3]) == "--camera-leaves" ||
                             std::string_view(argv[3]) == "--camera-aircraft" || std::string_view(argv[3]) == "--camera-command-list"))) &&
             std::string_view(argv[1]) == "--pid") {
    references = argc == 4 && std::string_view(argv[3]) == "--references";
    camera_callers = argc == 4 && std::string_view(argv[3]) == "--camera-callers";
    camera_context = argc == 4 && std::string_view(argv[3]) == "--camera-context";
    camera_methods = argc == 4 && std::string_view(argv[3]) == "--camera-methods";
    camera_view_setup = argc == 4 && std::string_view(argv[3]) == "--camera-view-setup";
    camera_view_state = argc == 4 && std::string_view(argv[3]) == "--camera-view-state";
    camera_service = argc == 4 && std::string_view(argv[3]) == "--camera-service";
    camera_leaves = argc == 4 && std::string_view(argv[3]) == "--camera-leaves";
    camera_aircraft = argc == 4 && std::string_view(argv[3]) == "--camera-aircraft";
    camera_command_list = argc == 4 && std::string_view(argv[3]) == "--camera-command-list";
    const std::string_view input(argv[2]);
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || pid == 0) {
      std::fprintf(stderr, "A nonzero decimal PID is required.\n");
      return 2;
    }
  } else if (argc != 1) {
    std::fprintf(stderr,
                 "Usage: camera-interface-inventory.exe [--self | --pid <FlightSimulator2024 PID> [--references | --camera-context | "
                 "--camera-methods | --camera-view-setup | --camera-view-state | --camera-callers | --camera-service | --camera-leaves | "
                 "--camera-aircraft | --camera-command-list] | --decoder-self-test]\n");
    return 2;
  }
  try {
    const auto start = std::chrono::steady_clock::now();
    const auto kind = camera_command_list ? CameraContextKind::command_list
                      : camera_aircraft   ? CameraContextKind::aircraft
                      : camera_leaves     ? CameraContextKind::leaves
                      : camera_service    ? CameraContextKind::service
                      : camera_view_state ? CameraContextKind::view_state
                      : camera_view_setup ? CameraContextKind::view_setup
                      : camera_methods    ? CameraContextKind::methods
                      : camera_context    ? CameraContextKind::creation
                                          : CameraContextKind::none;
    const auto result =
        self ? inspect_main_module(GetModuleHandleW(nullptr)) : inspect_simulator_main_module(pid, {}, references, kind, camera_callers);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    report(result, elapsed);
    return result.main_module_verified && result.image.valid_image &&
                   (!camera_command_list || (result.command_list_type.valid && result.command_list_type.available)) &&
                   (!(camera_view_setup || camera_view_state) || result.setup_literals.valid) &&
                   (!camera_view_state ||
                    (result.code_contract.valid && result.manager_slot_protection_valid && result.pose_constants.valid)) &&
                   (!camera_leaves || (result.callee_prefixes.valid && result.branch_prefixes.valid)) &&
                   (!camera_aircraft || (result.aircraft.valid && result.aircraft.selected && result.aircraft.method_rva != 0 &&
                                         result.aircraft_method_prefix.valid && result.aircraft_controller.valid &&
                                         result.aircraft_controller.object_present && result.aircraft_controller_method_prefix.valid &&
                                         result.aircraft_selected_object.valid && result.aircraft_selected_object.available &&
                                         result.aircraft_selected_object_method_prefix.valid && result.aircraft_component.valid &&
                                         result.aircraft_component.available && result.aircraft_component.component_present &&
                                         result.camera_keys.valid && result.camera_keys.camera_keys_inspected)) &&
                   (!camera_service ||
                    (result.renderer_service.valid && result.renderer_service.cached_present && result.renderer_service.method_rva != 0)) &&
                   (!(references || camera_callers) || (result.llvm_decoder_ready && result.references.valid_targets &&
                                                        result.references.error.empty() && result.references.read_failures == 0)) &&
                   (!(camera_context || camera_methods || camera_view_setup || camera_view_state || camera_service || camera_leaves ||
                      camera_aircraft || camera_command_list) ||
                    (result.llvm_decoder_ready && result.functions.valid_targets && result.functions.error.empty() &&
                     !result.functions.partial && result.manager_pointer_table.valid_table && result.manager_pointer_table.error.empty() &&
                     result.manager_pointer_table.read_failures == 0 && result.guard_metadata.valid && result.guard_metadata.error.empty()))
               ? 0
               : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Inventory failed: %s\n", error.what());
    return 1;
  }
}
