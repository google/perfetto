#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
#include "gtest/gtest.h"
#include "test/gtest_and_gmock.h"
#pragma clang diagnostic pop

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/protovm/vm_program.pb.h"
#include "protos/perfetto/trace/android/surfaceflinger_common.pb.h"
#include "protos/perfetto/trace/android/surfaceflinger_layers.pb.h"
#include "src/protovm/test/utils.h"
#include "src/protovm/vm.h"

namespace perfetto {
namespace protovm {
namespace test {

struct FieldMapping {
  std::vector<uint32_t> src_path;
  std::vector<uint32_t> dst_path;
};

struct NestedMergeNode {
  uint32_t field_id;
  std::vector<NestedMergeNode> children;
};

struct RepeatedMessageMapping {
  uint32_t src_list_field;
  std::vector<uint32_t> dst_path;
  uint32_t key_field;
  NestedMergeNode nested_merges;
};

struct TestLayer {
  int32_t id;
  std::optional<std::string> name;
};

struct TestDisplay {
  uint64_t id;
  std::optional<std::string> name;
};

static void FillTestLayer(protos::LayerProto* l, const TestLayer& layer) {
  l->set_id(layer.id);
  if (layer.name) {
    l->set_name(*layer.name);
  }
}

static void FillTestDisplay(protos::DisplayProto* d,
                            const TestDisplay& display) {
  d->set_id(display.id);
  if (display.name) {
    d->set_name(*display.name);
  }
}

static void AddTestLayer(protos::LayersSnapshotProto* snapshot,
                         const TestLayer& layer) {
  FillTestLayer(snapshot->mutable_layers()->add_layers(), layer);
}

static void AddTestLayer(protos::LayersPatchProto* patch,
                         const TestLayer& layer) {
  FillTestLayer(patch->add_layers(), layer);
}

static void AddTestDisplay(protos::LayersSnapshotProto* snapshot,
                           const TestDisplay& display) {
  FillTestDisplay(snapshot->add_displays(), display);
}

static void AddTestDisplay(protos::LayersPatchProto* patch,
                           const TestDisplay& display) {
  FillTestDisplay(patch->add_displays(), display);
}

static void VerifyLayer(const protos::LayersSnapshotProto& snapshot,
                        const TestLayer& expected) {
  bool found = false;
  for (const auto& layer : snapshot.layers().layers()) {
    if (layer.id() == expected.id) {
      found = true;
      EXPECT_EQ(layer.id(), expected.id);
      if (expected.name) {
        EXPECT_EQ(layer.name(), *expected.name);
      }
      break;
    }
  }
  ASSERT_TRUE(found) << "Layer with ID " << expected.id << " not found";
}

static void VerifyDisplay(const protos::LayersSnapshotProto& snapshot,
                          const TestDisplay& expected) {
  bool found = false;
  for (const auto& display : snapshot.displays()) {
    if (display.id() == expected.id) {
      found = true;
      EXPECT_EQ(display.id(), expected.id);
      if (expected.name) {
        EXPECT_EQ(display.name(), *expected.name);
      }
      break;
    }
  }
  ASSERT_TRUE(found) << "Display with ID " << expected.id << " not found";
}

static void VerifyLayerNotFound(const protos::LayersSnapshotProto& snapshot,
                                int32_t id) {
  for (const auto& layer : snapshot.layers().layers()) {
    EXPECT_NE(layer.id(), id)
        << "Layer with ID " << id << " should not be present";
  }
}

static void VerifyDisplayNotFound(const protos::LayersSnapshotProto& snapshot,
                                  uint64_t id) {
  for (const auto& display : snapshot.displays()) {
    EXPECT_NE(display.id(), id)
        << "Display with ID " << id << " should not be present";
  }
}

static void FillSelect(protos::VmInstruction* instr,
                       protos::VmCursorEnum cursor,
                       const std::vector<uint32_t>& path,
                       bool create_if_not_exist) {
  auto* sel = instr->mutable_select();
  sel->set_cursor(cursor);
  sel->set_create_if_not_exist(create_if_not_exist);
  for (uint32_t fid : path) {
    sel->add_relative_path()->set_field_id(fid);
  }
}

static protos::VmInstruction* AddSelect(protos::VmProgram& program,
                                        protos::VmCursorEnum cursor,
                                        const std::vector<uint32_t>& path,
                                        bool create_if_not_exist = false) {
  auto* instr = program.add_instructions();
  FillSelect(instr, cursor, path, create_if_not_exist);
  return instr;
}

static protos::VmInstruction* AddNestedSelect(
    protos::VmInstruction* parent,
    protos::VmCursorEnum cursor,
    const std::vector<uint32_t>& path,
    bool create_if_not_exist = false) {
  auto* instr = parent->add_nested_instructions();
  FillSelect(instr, cursor, path, create_if_not_exist);
  return instr;
}

static protos::VmInstruction* AddNestedMapSelect(
    protos::VmInstruction* parent,
    const std::vector<uint32_t>& path,
    uint32_t key_field_id,
    uint32_t register_to_match,
    bool create_if_not_exist = false) {
  auto* instr = AddNestedSelect(parent, protos::VmCursorEnum::VM_CURSOR_DST,
                                path, create_if_not_exist);
  auto* sel = instr->mutable_select();
  auto* map_key_comp = sel->add_relative_path();
  map_key_comp->set_map_key_field_id(key_field_id);
  map_key_comp->set_register_to_match(register_to_match);
  return instr;
}

static protos::VmInstruction* AddSelectRepeated(protos::VmProgram& program,
                                                protos::VmCursorEnum cursor,
                                                uint32_t field_id) {
  auto* instr = program.add_instructions();
  auto* sel = instr->mutable_select();
  sel->set_cursor(cursor);
  auto* comp = sel->add_relative_path();
  comp->set_field_id(field_id);
  comp->set_is_repeated(true);
  return instr;
}

static void AddRegLoad(protos::VmInstruction* parent, uint32_t dst_register) {
  parent->add_nested_instructions()->mutable_reg_load()->set_dst_register(
      dst_register);
}

static void AddSet(protos::VmInstruction* parent) {
  parent->add_nested_instructions()->mutable_set();
}

static void AddDel(protos::VmInstruction* parent) {
  parent->add_nested_instructions()->mutable_del();
}

static void AddMerge(protos::VmInstruction* parent,
                     bool skip_submessages = false) {
  parent->add_nested_instructions()->mutable_merge()->set_skip_submessages(
      skip_submessages);
}

static void AddDeleteByKey(protos::VmProgram& program,
                           uint32_t src_list_field,
                           const std::vector<uint32_t>& dst_path,
                           uint32_t key_field_id) {
  auto* instr_src_sel = AddSelectRepeated(
      program, protos::VmCursorEnum::VM_CURSOR_SRC, src_list_field);
  AddRegLoad(instr_src_sel, 1);

  auto* instr_dst_sel =
      AddNestedMapSelect(instr_src_sel, dst_path, key_field_id, 1);
  AddDel(instr_dst_sel);
}

static void AddPrimitiveMapping(protos::VmProgram& program,
                                const std::vector<uint32_t>& src_path,
                                const std::vector<uint32_t>& dst_path) {
  auto* src_instr =
      AddSelect(program, protos::VmCursorEnum::VM_CURSOR_SRC, src_path);
  auto* dst_instr =
      AddNestedSelect(src_instr, protos::VmCursorEnum::VM_CURSOR_DST, dst_path);
  AddSet(dst_instr);
}

static void AddRecursiveMerge(protos::VmInstruction* current_instr,
                              const NestedMergeNode& node) {
  for (const auto& child : node.children) {
    auto* src_sub = AddNestedSelect(
        current_instr, protos::VmCursorEnum::VM_CURSOR_SRC, {child.field_id});
    src_sub->set_abort_level(
        ::perfetto::protos::VmInstruction_AbortLevel_SKIP_CURRENT_INSTRUCTION);

    auto* dst_sub =
        AddNestedSelect(src_sub, protos::VmCursorEnum::VM_CURSOR_DST,
                        {child.field_id}, true /* create_if_not_exist */);
    AddRecursiveMerge(dst_sub, child);
  }

  AddMerge(current_instr, true /* skip_submessages */);
}

static void AddMappedMessagePatch(protos::VmProgram& program,
                                  const RepeatedMessageMapping& mapping) {
  auto* instr_src_sel = AddSelectRepeated(
      program, protos::VmCursorEnum::VM_CURSOR_SRC, mapping.src_list_field);
  auto* instr_key_sel = AddNestedSelect(
      instr_src_sel, protos::VmCursorEnum::VM_CURSOR_SRC, {mapping.key_field});
  AddRegLoad(instr_key_sel, 1);
  auto* instr_dst_sel =
      AddNestedMapSelect(instr_src_sel, mapping.dst_path, mapping.key_field, 1,
                         true /* create_if_not_exist */);

  AddRecursiveMerge(instr_dst_sel, mapping.nested_merges);
}

static protos::VmProgram LayersProgram() {
  protos::VmProgram program;

  const std::vector<FieldMapping> root_mappings = {
      {{protos::LayersPatchProto::kElapsedRealtimeNanosFieldNumber},
       {protos::LayersSnapshotProto::kElapsedRealtimeNanosFieldNumber}},
      {{protos::LayersPatchProto::kWhereFieldNumber},
       {protos::LayersSnapshotProto::kWhereFieldNumber}},
      {{protos::LayersPatchProto::kHwcBlobFieldNumber},
       {protos::LayersSnapshotProto::kHwcBlobFieldNumber}},
      {{protos::LayersPatchProto::kExcludesCompositionStateFieldNumber},
       {protos::LayersSnapshotProto::kExcludesCompositionStateFieldNumber}},
      {{protos::LayersPatchProto::kMissedEntriesFieldNumber},
       {protos::LayersSnapshotProto::kMissedEntriesFieldNumber}},
      {{protos::LayersPatchProto::kVsyncIdFieldNumber},
       {protos::LayersSnapshotProto::kVsyncIdFieldNumber}},
  };

  for (const auto& m : root_mappings) {
    AddPrimitiveMapping(program, m.src_path, m.dst_path);
  }

  // Process layers
  RepeatedMessageMapping layer_mapping;
  layer_mapping.src_list_field = protos::LayersPatchProto::kLayersFieldNumber;
  layer_mapping.dst_path = {protos::LayersSnapshotProto::kLayersFieldNumber,
                            protos::LayersProto::kLayersFieldNumber};
  layer_mapping.key_field = protos::LayerProto::kIdFieldNumber;
  layer_mapping.nested_merges = {
      0,  // dummy
      {{protos::LayerProto::kPositionFieldNumber, {}},
       {protos::LayerProto::kInputWindowInfoFieldNumber,
        {{protos::InputWindowInfoProto::kTouchableRegionCropFieldNumber,
          {}}}}}};
  AddMappedMessagePatch(program, layer_mapping);

  // Process displays
  RepeatedMessageMapping display_mapping;
  display_mapping.src_list_field =
      protos::LayersPatchProto::kDisplaysFieldNumber;
  display_mapping.dst_path = {
      protos::LayersSnapshotProto::kDisplaysFieldNumber};
  display_mapping.key_field = protos::DisplayProto::kIdFieldNumber;
  display_mapping.nested_merges = {0, {}};  // Empty nested merges
  AddMappedMessagePatch(program, display_mapping);

  // Process deleted_layer_ids
  AddDeleteByKey(program, protos::LayersPatchProto::kDeletedLayerIdsFieldNumber,
                 {protos::LayersSnapshotProto::kLayersFieldNumber,
                  protos::LayersProto::kLayersFieldNumber},
                 protos::LayerProto::kIdFieldNumber);

  // Process deleted_display_ids
  AddDeleteByKey(program,
                 protos::LayersPatchProto::kDeletedDisplayIdsFieldNumber,
                 {protos::LayersSnapshotProto::kDisplaysFieldNumber},
                 protos::DisplayProto::kIdFieldNumber);

  return program;
}

class VmSurfaceFlingerTest : public ::testing::Test {
 protected:
  static constexpr size_t MEMORY_LIMIT_BYTES =
      static_cast<const size_t>(10 * 1024 * 1024);

  std::string SerializeIncrementalStateAsString(const Vm& vm) const {
    protozero::HeapBuffered<protozero::Message> proto;
    vm.SerializeIncrementalState(proto.get());
    return proto.SerializeAsString();
  }
};

TEST_F(VmSurfaceFlingerTest, Full) {
  auto program = LayersProgram().SerializeAsString();

  protos::LayersSnapshotProto initial_state;
  initial_state.set_elapsed_realtime_nanos(123456L);
  initial_state.set_where("visibleRegionsDirty");
  initial_state.set_hwc_blob("maxDownScale: 4");
  initial_state.set_excludes_composition_state(false);
  initial_state.set_missed_entries(1);
  initial_state.set_vsync_id(1);

  // Add initial layers: 1, 2, 3
  AddTestLayer(&initial_state, {1, "Layer1"});
  AddTestLayer(&initial_state, {2, "Layer2"});
  AddTestLayer(&initial_state, {3, "Layer3"});

  // Set position, input_window_info, children, and metadata for Layer 3 in
  // initial state
  auto* l3 = initial_state.mutable_layers()->mutable_layers(2);
  l3->mutable_position()->set_x(10.0f);
  l3->mutable_position()->set_y(20.0f);

  auto* win_info = l3->mutable_input_window_info();
  win_info->mutable_touchable_region_crop()->set_left(5);
  win_info->mutable_touchable_region_crop()->set_top(5);
  win_info->mutable_touchable_region_crop()->set_right(10);
  win_info->mutable_touchable_region_crop()->set_bottom(10);

  l3->add_children(10);
  l3->add_children(20);
  (*l3->mutable_metadata())[1] = "v1";

  // Add initial displays: 1, 2, 3
  AddTestDisplay(&initial_state, {1, "Display1"});
  AddTestDisplay(&initial_state, {2, "Display2"});
  AddTestDisplay(&initial_state, {3, "Display3"});

  Vm vm{AsConstBytes(program), MEMORY_LIMIT_BYTES,
        AsConstBytes(initial_state.SerializeAsString())};

  protos::LayersPatchProto patch;
  patch.set_elapsed_realtime_nanos(999999L);
  patch.set_where("bufferLatched");
  patch.set_hwc_blob("maxDownScale: 10");
  patch.set_excludes_composition_state(true);
  patch.set_missed_entries(2);
  patch.set_vsync_id(2);

  // Patch layers: 1 (no change), 3 (updated), 4 (new)
  AddTestLayer(&patch, {3, "Layer3_updated"});

  // Update position, input_window_info, children, and metadata for Layer 3 in
  // patch
  auto* patch_l3 = patch.mutable_layers(0);
  patch_l3->mutable_position()->set_y(30.0f);
  patch_l3->mutable_input_window_info()
      ->mutable_touchable_region_crop()
      ->set_left(2);

  patch_l3->add_children(30);
  (*patch_l3->mutable_metadata())[2] = "v2";

  AddTestLayer(&patch, {4, "Layer4"});

  // Patch displays: 1 (no change), 3 (updated), 4 (new)
  AddTestDisplay(&patch, {3, "Display3_updated"});
  AddTestDisplay(&patch, {4, "Display4"});

  // Deletions
  patch.add_deleted_layer_ids(2);
  patch.add_deleted_display_ids(2);

  vm.ApplyPatch(AsConstBytes(patch.SerializeAsString()));

  protos::LayersSnapshotProto updated_state;
  updated_state.ParseFromString(SerializeIncrementalStateAsString(vm));

  ASSERT_EQ(updated_state.elapsed_realtime_nanos(), 999999L);
  ASSERT_EQ(updated_state.where(), "bufferLatched");
  ASSERT_EQ(updated_state.hwc_blob(), "maxDownScale: 10");
  ASSERT_EQ(updated_state.excludes_composition_state(), true);
  ASSERT_EQ(updated_state.missed_entries(), 2U);
  ASSERT_EQ(updated_state.vsync_id(), 2);

  VerifyLayer(updated_state, {1, "Layer1"});
  VerifyLayer(updated_state, {3, "Layer3_updated"});
  VerifyLayer(updated_state, {4, "Layer4"});
  VerifyLayerNotFound(updated_state, 2);

  // Verify position and input_window_info for Layer 3
  const protos::LayerProto* updated_l3 = nullptr;
  for (const auto& layer : updated_state.layers().layers()) {
    if (layer.id() == 3) {
      updated_l3 = &layer;
      break;
    }
  }
  ASSERT_NE(updated_l3, nullptr);
  ASSERT_TRUE(updated_l3->has_position());
  EXPECT_FLOAT_EQ(updated_l3->position().x(), 10.0f);
  EXPECT_FLOAT_EQ(updated_l3->position().y(), 30.0f);

  ASSERT_TRUE(updated_l3->has_input_window_info());
  ASSERT_TRUE(updated_l3->input_window_info().has_touchable_region_crop());
  EXPECT_EQ(updated_l3->input_window_info().touchable_region_crop().left(), 2);
  EXPECT_EQ(updated_l3->input_window_info().touchable_region_crop().top(), 5);
  EXPECT_EQ(updated_l3->input_window_info().touchable_region_crop().right(),
            10);
  EXPECT_EQ(updated_l3->input_window_info().touchable_region_crop().bottom(),
            10);

  // Verify children
  ASSERT_EQ(updated_l3->children_size(), 1);
  EXPECT_EQ(updated_l3->children(0), 30);

  // Verify metadata
  ASSERT_EQ(updated_l3->metadata_size(), 1);
  EXPECT_EQ(updated_l3->metadata().at(2), "v2");

  VerifyDisplay(updated_state, {1, "Display1"});
  VerifyDisplay(updated_state, {3, "Display3_updated"});
  VerifyDisplay(updated_state, {4, "Display4"});
  VerifyDisplayNotFound(updated_state, 2);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
