// Compile-only probe against pinned, unmodified ReShade and ImGui headers.
#include <imgui.h>
#include <reshade_api.hpp>
#define RESHADE_API_LIBRARY_EXPORT
#include <reshade_overlay.hpp>
#include "native_bridge.h"

namespace api = reshade::api;

extern "C" uint64_t probe_native(api::api_object* object) {
  return object->get_native();
}
extern "C" api::device* probe_device(api::device_object* object) {
  return object->get_device();
}
extern "C" api::command_list* probe_immediate_list(api::command_queue* queue) {
  return queue->get_immediate_command_list();
}
extern "C" api::device_api probe_api(api::device* device) {
  return device->get_api();
}
extern "C" void probe_private_get(api::api_object* object, const uint8_t* id, uint64_t* value) {
  object->get_private_data(id, value);
}
extern "C" void probe_private_set(api::api_object* object, const uint8_t* id, uint64_t value) {
  object->set_private_data(id, value);
}
extern "C" void probe_draw(api::command_list* list, uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) {
  list->draw(count, instances, first, first_instance);
}
extern "C" void probe_indexed(api::command_list* list,
                              uint32_t count,
                              uint32_t instances,
                              uint32_t first,
                              int32_t offset,
                              uint32_t first_instance) {
  list->draw_indexed(count, instances, first, offset, first_instance);
}
extern "C" void probe_clear(api::command_list* list, api::resource_view view, const float* color, uint32_t count, const api::rect* rects) {
  list->clear_render_target_view(view, color, count, rects);
}
extern "C" bool probe_create_view(api::device* device,
                                  api::resource resource,
                                  api::resource_usage usage,
                                  const api::resource_view_desc& desc,
                                  api::resource_view* out) {
  return device->create_resource_view(resource, usage, desc, out);
}
extern "C" void probe_destroy_view(api::device* device, api::resource_view view) {
  device->destroy_resource_view(view);
}
extern "C" void probe_copy_resource(api::command_list* list, api::resource source, api::resource destination) {
  list->copy_resource(source, destination);
}
extern "C" void probe_copy_texture(api::command_list* list,
                                   api::resource source,
                                   uint32_t source_subresource,
                                   const api::subresource_box* source_box,
                                   api::resource destination,
                                   uint32_t destination_subresource,
                                   const api::subresource_box* destination_box,
                                   api::filter_mode filter) {
  list->copy_texture_region(source, source_subresource, source_box, destination, destination_subresource, destination_box, filter);
}
extern "C" void probe_wait_idle(api::command_queue* queue) {
  queue->wait_idle();
}
extern "C" bool probe_open_overlay(api::effect_runtime* runtime, bool open, api::input_source source) {
  return runtime->open_overlay(open, source);
}
extern "C" uint64_t probe_bridge_resource(api::device* device, uint64_t view) {
  return taxi_camera_resource_from_view(device, view);
}
extern "C" void probe_bridge_desc(api::device* device, uint64_t view, api::resource_view_desc* out) {
  taxi_camera_resource_view_desc(device, view, out);
}

// Observe every parameter, including arguments passed on the stack. These
// adapters compare the incoming ABI used by the registered free callbacks.
extern "C" void observe(void*, uint64_t, const void*, uint32_t, uint64_t);
extern "C" void probe_init_resource(api::device* device,
                                    const api::resource_desc& desc,
                                    const api::subresource_data* data,
                                    api::resource_usage usage,
                                    api::resource resource) {
  observe(device, resource.handle, &desc, static_cast<uint32_t>(usage), reinterpret_cast<uint64_t>(data));
}
extern "C" void probe_init_view(api::device* device,
                                api::resource resource,
                                api::resource_usage usage,
                                const api::resource_view_desc& desc,
                                api::resource_view view) {
  observe(device, resource.handle, &desc, static_cast<uint32_t>(usage), view.handle);
}
extern "C" void probe_destroy_resource(api::device* device, api::resource resource) {
  observe(device, resource.handle, nullptr, 0, 0);
}
extern "C" void probe_bind(api::command_list* list, uint32_t count, const api::resource_view* views, api::resource_view dsv) {
  observe(list, dsv.handle, views, count, 0);
}
extern "C" bool observe_copy(void*, uint64_t, uint32_t, const void*, uint64_t, uint32_t, const void*, uint32_t);
extern "C" bool observe_upload(void*, uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint32_t, const void*);
extern "C" bool observe_resolve(void*, uint64_t, uint32_t, const void*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" bool observe_end(void*);
extern "C" bool probe_event_copy_resource(api::command_list* list, api::resource source, api::resource destination) {
  return observe_copy(list, source.handle, 0, nullptr, destination.handle, 0, nullptr, 0);
}
extern "C" bool probe_event_copy_texture(api::command_list* list,
                                         api::resource source,
                                         uint32_t source_subresource,
                                         const api::subresource_box* source_box,
                                         api::resource destination,
                                         uint32_t destination_subresource,
                                         const api::subresource_box* destination_box,
                                         api::filter_mode filter) {
  return observe_copy(list, source.handle, source_subresource, source_box, destination.handle, destination_subresource, destination_box,
                      static_cast<uint32_t>(filter));
}
extern "C" bool probe_event_upload(api::command_list* list,
                                   api::resource source,
                                   uint64_t offset,
                                   uint32_t row_length,
                                   uint32_t slice_height,
                                   api::resource destination,
                                   uint32_t destination_subresource,
                                   const api::subresource_box* destination_box) {
  return observe_upload(list, source.handle, offset, row_length, slice_height, destination.handle, destination_subresource,
                        destination_box);
}
extern "C" bool probe_event_resolve(api::command_list* list,
                                    api::resource source,
                                    uint32_t source_subresource,
                                    const api::subresource_box* source_box,
                                    api::resource destination,
                                    uint32_t destination_subresource,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t z,
                                    api::format format) {
  return observe_resolve(list, source.handle, source_subresource, source_box, destination.handle, destination_subresource, x, y, z,
                         static_cast<uint32_t>(format));
}
extern "C" bool probe_event_begin_pass(api::command_list* list,
                                       uint32_t count,
                                       const api::render_pass_render_target_desc* rts,
                                       const api::render_pass_depth_stencil_desc* ds,
                                       api::render_pass_flags flags) {
  observe(list, count, rts, static_cast<uint32_t>(flags), reinterpret_cast<uint64_t>(ds));
  return false;
}
extern "C" bool probe_event_end_pass(api::command_list* list) {
  return observe_end(list);
}
extern "C" void probe_event_destroy_queue(api::command_queue* queue) {
  observe(queue, 0, nullptr, 0, 0);
}

// The PFD adapter observes these additional ReShade state callbacks. Consume
// every argument, including stack parameters; callback input ABI is distinct
// from the known-incompatible virtual aggregate-return methods below.
extern "C" void observe_state(void*, uint64_t, uint64_t, uint64_t, const void*, uint64_t, const void*, uint64_t);
extern "C" void observe_ranges(void*, uint32_t, uint32_t, const void*);
extern "C" void observe_constants(void*, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, const void*);
extern "C" void observe_descriptors(void*, uint32_t, uint64_t, uint32_t, const void*);
extern "C" void observe_tables(void*, uint32_t, uint64_t, uint32_t, uint32_t, const void*, uint32_t, const uint32_t*);
extern "C" void probe_event_init_layout(api::device* device,
                                        uint32_t count,
                                        const api::pipeline_layout_param* params,
                                        api::pipeline_layout layout) {
  observe_state(device, count, layout.handle, 0, params, 0, nullptr, 0);
}
extern "C" void probe_event_destroy_layout(api::device* device, api::pipeline_layout layout) {
  observe_state(device, layout.handle, 0, 0, nullptr, 0, nullptr, 0);
}
extern "C" void probe_event_init_pipeline(api::device* device,
                                          api::pipeline_layout layout,
                                          uint32_t count,
                                          const api::pipeline_subobject* subobjects,
                                          api::pipeline pipeline) {
  observe_state(device, layout.handle, count, pipeline.handle, subobjects, 0, nullptr, 0);
}
extern "C" void probe_event_destroy_pipeline(api::device* device, api::pipeline pipeline) {
  observe_state(device, pipeline.handle, 0, 0, nullptr, 0, nullptr, 0);
}
extern "C" void probe_event_bind_pipeline(api::command_list* list, api::pipeline_stage stages, api::pipeline pipeline) {
  observe_state(list, static_cast<uint64_t>(stages), pipeline.handle, 0, nullptr, 0, nullptr, 0);
}
extern "C" void probe_event_bind_states(api::command_list* list, uint32_t count, const api::dynamic_state* states, const uint32_t* values) {
  observe_state(list, count, 0, 0, states, 0, values, 0);
}
extern "C" void probe_event_viewports(api::command_list* list, uint32_t first, uint32_t count, const api::viewport* values) {
  observe_ranges(list, first, count, values);
}
extern "C" void probe_event_scissors(api::command_list* list, uint32_t first, uint32_t count, const api::rect* values) {
  observe_ranges(list, first, count, values);
}
extern "C" void probe_event_constants(api::command_list* list,
                                      api::shader_stage stages,
                                      api::pipeline_layout layout,
                                      uint32_t index,
                                      uint32_t first,
                                      uint32_t count,
                                      const void* data) {
  observe_constants(list, static_cast<uint32_t>(stages), layout.handle, index, first, count, data);
}
extern "C" void probe_event_descriptors(api::command_list* list,
                                        api::shader_stage stages,
                                        api::pipeline_layout layout,
                                        uint32_t index,
                                        const api::descriptor_table_update& update) {
  observe_descriptors(list, static_cast<uint32_t>(stages), layout.handle, index, &update);
}
extern "C" void probe_event_tables(api::command_list* list,
                                   api::shader_stage stages,
                                   api::pipeline_layout layout,
                                   uint32_t first,
                                   uint32_t count,
                                   const api::descriptor_table* tables,
                                   uint32_t dynamic_count,
                                   const uint32_t* dynamic_offsets) {
  observe_tables(list, static_cast<uint32_t>(stages), layout.handle, first, count, tables, dynamic_count, dynamic_offsets);
}
extern "C" void probe_event_bundle(api::command_list* first, api::command_list* second) {
  observe_state(first, 0, 0, 0, second, 0, nullptr, 0);
}
extern "C" bool probe_event_indirect(api::command_list* list,
                                     api::indirect_command kind,
                                     api::resource buffer,
                                     uint64_t offset,
                                     uint32_t count,
                                     uint32_t stride) {
  observe_state(list, static_cast<uint64_t>(kind), buffer.handle, offset, nullptr, count, nullptr, stride);
  return false;
}
extern "C" void probe_event_reset(api::command_list* list) {
  observe_state(list, 0, 0, 0, nullptr, 0, nullptr, 0);
}
extern "C" void probe_event_destroy_device(api::device* device) {
  observe_state(device, 0, 0, 0, nullptr, 0, nullptr, 0);
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
// Compiler-specific layout inquiry is intentional for this inherited SDK type.
// These constants compare the actual consumed fields without asserting that
// the type is standard-layout in portable C++.
extern "C" void probe_flagged_range(uint64_t* out) {
  out[0] = offsetof(api::descriptor_range_with_flags, count);
  out[1] = offsetof(api::descriptor_range_with_flags, type);
  out[2] = offsetof(api::descriptor_range_with_flags, flags);
  out[3] = offsetof(api::descriptor_range_with_flags, static_samplers);
}
#pragma clang diagnostic pop
extern "C" void probe_pfd_layout(uint64_t* out) {
  out[0] = sizeof(api::pipeline_layout_param);
  out[1] = alignof(api::pipeline_layout_param);
  out[2] = offsetof(api::pipeline_layout_param, type);
  out[3] = offsetof(api::pipeline_layout_param, push_constants);
  out[4] = offsetof(api::pipeline_layout_param, push_descriptors);
  out[5] = offsetof(api::pipeline_layout_param, descriptor_table.count);
  out[6] = offsetof(api::pipeline_layout_param, descriptor_table.ranges);
  out[7] = offsetof(api::pipeline_layout_param, descriptor_table_with_flags.count);
  out[8] = offsetof(api::pipeline_layout_param, descriptor_table_with_flags.ranges);
  out[9] = sizeof(api::constant_range);
  out[10] = offsetof(api::constant_range, count);
  out[11] = sizeof(api::descriptor_range);
  out[12] = offsetof(api::descriptor_range, count);
  out[13] = offsetof(api::descriptor_range, type);
  out[14] = sizeof(api::descriptor_range_with_flags);
  out[15] = alignof(api::descriptor_range_with_flags);
  out[16] = sizeof(api::descriptor_table_update);
  out[17] = alignof(api::descriptor_table_update);
  out[18] = offsetof(api::descriptor_table_update, binding);
  out[19] = offsetof(api::descriptor_table_update, array_offset);
  out[20] = offsetof(api::descriptor_table_update, count);
  out[21] = offsetof(api::descriptor_table_update, type);
  out[22] = offsetof(api::descriptor_table_update, descriptors);
  out[23] = sizeof(api::viewport);
  out[24] = alignof(api::viewport);
  out[25] = offsetof(api::viewport, x);
  out[26] = offsetof(api::viewport, y);
  out[27] = offsetof(api::viewport, width);
  out[28] = offsetof(api::viewport, height);
  out[29] = offsetof(api::viewport, min_depth);
  out[30] = offsetof(api::viewport, max_depth);
  out[31] = sizeof(api::pipeline_layout);
  out[32] = sizeof(api::pipeline);
  out[33] = sizeof(api::descriptor_table);
  out[34] = sizeof(api::pipeline_stage);
  out[35] = sizeof(api::shader_stage);
  out[36] = sizeof(api::dynamic_state);
}

extern "C" bool probe_ui_input(imgui_function_table* table, const char* label, int* value) {
  return table->InputInt(label, value, 1, 100, 0);
}
extern "C" bool probe_ui_checkbox(imgui_function_table* table, const char* label, bool* value) {
  return table->Checkbox(label, value);
}
extern "C" bool probe_ui_slider_float(imgui_function_table* table,
                                      const char* label,
                                      float* value,
                                      float minimum,
                                      float maximum,
                                      const char* format) {
  return table->SliderFloat(label, value, minimum, maximum, format, 0);
}
extern "C" bool probe_ui_drag_float3(imgui_function_table* table,
                                     const char* label,
                                     float* values,
                                     float speed,
                                     float minimum,
                                     float maximum,
                                     const char* format) {
  return table->DragFloat3(label, values, speed, minimum, maximum, format, 0);
}
extern "C" void probe_ui_push_id_int(imgui_function_table* table, int id) {
  // ImGui::PushID(static_cast<int>(i)) selects PushID4 in the pinned overlay.
  table->PushID4(id);
}
extern "C" void probe_ui_pop_id(imgui_function_table* table) {
  table->PopID();
}
extern "C" bool probe_ui_collapsing_header(imgui_function_table* table, const char* label) {
  return table->CollapsingHeader(label, 0);
}
extern "C" bool probe_ui_child(imgui_function_table* table, const char* label, const ImVec2& size) {
  return table->BeginChild(label, size, 1, 0);
}
extern "C" bool probe_ui_selectable(imgui_function_table* table, const char* label, bool selected, const ImVec2& size) {
  return table->Selectable(label, selected, 0, size);
}
extern "C" bool probe_ui_button(imgui_function_table* table, const char* label, const ImVec2& size) {
  return table->Button(label, size);
}
extern "C" void probe_ui_disabled(imgui_function_table* table, bool disabled) {
  table->BeginDisabled(disabled);
}
extern "C" void probe_ui_end_disabled(imgui_function_table* table) {
  table->EndDisabled();
}
extern "C" void probe_ui_end_child(imgui_function_table* table) {
  table->EndChild();
}
extern "C" void probe_ui_same_line(imgui_function_table* table, float offset, float spacing) {
  table->SameLine(offset, spacing);
}
extern "C" void probe_ui_text_unformatted(imgui_function_table* table, const char* text) {
  table->TextUnformatted(text, nullptr);
}
extern "C" void probe_ui_text(imgui_function_table* table, const char* format, ...) {
  va_list args;
  va_start(args, format);
  table->TextV(format, args);
  va_end(args);
}
extern "C" void probe_ui_text_wrapped(imgui_function_table* table, const char* format, ...) {
  va_list args;
  va_start(args, format);
  table->TextWrappedV(format, args);
  va_end(args);
}

// Constant values in emitted assembly compare the referenced data layouts.
extern "C" void probe_layout(uint64_t* out) {
  out[0] = sizeof(api::resource_desc);
  out[1] = sizeof(api::resource_view_desc);
  out[2] = sizeof(api::subresource_data);
  out[3] = sizeof(api::rect);
  out[4] = offsetof(api::resource_desc, texture);
  out[5] = offsetof(api::resource_desc, heap);
  out[6] = offsetof(api::resource_desc, usage);
  out[7] = offsetof(api::resource_desc, flags);
  out[8] = offsetof(api::resource_view_desc, texture);
  out[9] = sizeof(ImVec2);
  out[10] = sizeof(imgui_function_table);
  out[11] = offsetof(api::resource_desc, type);
  out[12] = offsetof(api::resource_desc, texture.width);
  out[13] = offsetof(api::resource_desc, texture.height);
  out[14] = offsetof(api::resource_desc, texture.depth_or_layers);
  out[15] = offsetof(api::resource_desc, texture.levels);
  out[16] = offsetof(api::resource_desc, texture.samples);
  out[17] = offsetof(api::resource_desc, texture.format);
  out[18] = offsetof(api::resource_view_desc, type);
  out[19] = offsetof(api::resource_view_desc, format);
  out[20] = offsetof(api::resource_view_desc, texture.first_level);
  out[21] = offsetof(api::resource_view_desc, texture.levels);
  out[22] = offsetof(api::resource_view_desc, texture.first_layer);
  out[23] = offsetof(api::resource_view_desc, texture.layers);
  out[24] = alignof(api::resource_desc);
  out[25] = alignof(api::resource_view_desc);
  out[26] = sizeof(api::resource);
  out[27] = sizeof(api::resource_view);
  out[28] = offsetof(ImVec2, x);
  out[29] = offsetof(ImVec2, y);
  out[30] = sizeof(api::render_pass_render_target_desc);
  out[31] = alignof(api::render_pass_render_target_desc);
  out[32] = offsetof(api::render_pass_render_target_desc, view);
  out[33] = sizeof(api::subresource_box);
  out[34] = alignof(api::subresource_box);
  out[35] = offsetof(api::subresource_box, left);
  out[36] = offsetof(api::subresource_box, top);
  out[37] = offsetof(api::subresource_box, front);
  out[38] = offsetof(api::subresource_box, right);
  out[39] = offsetof(api::subresource_box, bottom);
  out[40] = offsetof(api::subresource_box, back);
}
