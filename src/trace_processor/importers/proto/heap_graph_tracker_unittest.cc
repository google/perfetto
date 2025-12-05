/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/proto/heap_graph_tracker.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "protos/perfetto/trace/profiling/heap_graph.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/util/profiler_util.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::UnorderedElementsAre;

TEST(HeapGraphTrackerTest, PackageFromLocationApp) {
  std::unique_ptr<TraceStorage> storage(new TraceStorage());

  const char data_app_path[] =
      "/data/app/org.perfetto.test-6XfQhnaSkFwGK0sYL9is0G==/base.apk";
  EXPECT_EQ(PackageFromLocation(storage.get(), data_app_path),
            "org.perfetto.test");

  const char with_extra_dir[] =
      "/data/app/~~ASDFGH1234QWerT==/"
      "com.perfetto.test-MNBVCX7890SDTst6==/test.apk";
  EXPECT_EQ(PackageFromLocation(storage.get(), with_extra_dir),
            "com.perfetto.test");

  const char odex[] =
      "/data/app/com.google.android.apps.wellbeing-"
      "qfQCaB4uJ7P0OPpZQqOu0Q==/oat/arm64/base.odex";
  EXPECT_EQ(PackageFromLocation(storage.get(), odex),
            "com.google.android.apps.wellbeing");

  const char inmem_dex[] =
      "[anon:dalvik-classes.dex extracted in memory from "
      "/data/app/~~uUgHYtbjPNr2VFa3byIF4Q==/"
      "com.perfetto.example-aC94wTfXRC60l2HJU5YvjQ==/base.apk]";
  EXPECT_EQ(PackageFromLocation(storage.get(), inmem_dex),
            "com.perfetto.example");
}

TEST(HeapGraphTrackerTest, PopulateNativeSize) {
  constexpr uint64_t kSeqId = 1;
  constexpr UniquePid kPid = 1;
  constexpr int64_t kTimestamp = 1;

  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  context.process_tracker = std::make_unique<ProcessTracker>(&context);
  context.process_tracker->GetOrCreateProcess(kPid);

  HeapGraphTracker tracker(context.storage.get());

  constexpr uint64_t kLocation = 0;
  tracker.AddInternedLocationName(kSeqId, kLocation,
                                  context.storage->InternString("location"));

  enum Fields : uint8_t { kReferent = 1, kThunk, kThis0, kNext };

  tracker.AddInternedFieldName(kSeqId, kReferent,
                               "java.lang.ref.Reference.referent");
  tracker.AddInternedFieldName(kSeqId, kThunk, "sun.misc.Cleaner.thunk");
  tracker.AddInternedFieldName(
      kSeqId, kThis0,
      "libcore.util.NativeAllocationRegistry$CleanerThunk.this$0");
  tracker.AddInternedFieldName(kSeqId, kNext, "sun.misc.Cleaner.next");

  enum Types : uint8_t {
    kTypeBitmap = 1,
    kTypeCleaner,
    kTypeCleanerThunk,
    kTypeNativeAllocationRegistry,
  };

  tracker.AddInternedType(
      kSeqId, kTypeBitmap,
      context.storage->InternString("android.graphics.Bitmap"), kLocation,
      /*object_size=*/0,
      /*field_name_ids=*/{}, /*superclass_id=*/0,
      /*classloader_id=*/0, /*no_fields=*/false,
      protos::pbzero::HeapGraphType::KIND_NORMAL);

  tracker.AddInternedType(kSeqId, kTypeCleaner,
                          context.storage->InternString("sun.misc.Cleaner"),
                          kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{kReferent, kThunk, kNext},
                          /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);

  tracker.AddInternedType(
      kSeqId, kTypeCleanerThunk,
      context.storage->InternString(
          "libcore.util.NativeAllocationRegistry$CleanerThunk"),
      kLocation, /*object_size=*/0,
      /*field_name_ids=*/{kThis0}, /*superclass_id=*/0,
      /*classloader_id=*/0, /*no_fields=*/false,
      protos::pbzero::HeapGraphType::KIND_NORMAL);

  tracker.AddInternedType(
      kSeqId, kTypeNativeAllocationRegistry,
      context.storage->InternString("libcore.util.NativeAllocationRegistry"),
      kLocation, /*object_size=*/0,
      /*field_name_ids=*/{}, /*superclass_id=*/0,
      /*classloader_id=*/0, /*no_fields=*/false,
      protos::pbzero::HeapGraphType::KIND_NORMAL);

  enum Objects : uint8_t {
    kObjBitmap = 1,
    kObjCleaner,
    kObjThunk,
    kObjNativeAllocationRegistry,
  };

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = kObjBitmap;
    obj.type_id = kTypeBitmap;

    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = kObjCleaner;
    obj.type_id = kTypeCleaner;
    obj.referred_objects = {kObjBitmap, kObjThunk, 0};

    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = kObjThunk;
    obj.type_id = kTypeCleanerThunk;
    obj.referred_objects = {kObjNativeAllocationRegistry};

    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = kObjNativeAllocationRegistry;
    obj.type_id = kTypeNativeAllocationRegistry;

    // NativeAllocationRegistry.size least significant bit is used to encode the
    // source of the allocation (1: malloc, 0: other).
    obj.native_allocation_registry_size = 24242 | 1;

    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  tracker.FinalizeProfile(kSeqId);

  const auto& objs_table = context.storage->heap_graph_object_table();
  const auto& class_table = context.storage->heap_graph_class_table();
  size_t count_bitmaps = 0;
  for (auto it = objs_table.IterateRows(); it; ++it) {
    auto class_row = class_table.FindById(it.type_id());
    ASSERT_TRUE(class_row.has_value());
    if (context.storage->string_pool().Get(class_row->name()) ==
        "android.graphics.Bitmap") {
      EXPECT_EQ(it.native_size(), 24242);
      count_bitmaps++;
    } else {
      EXPECT_EQ(it.native_size(), 0)
          << context.storage->string_pool().Get(class_row->name()).c_str()
          << " has non zero native_size";
    }
  }
  EXPECT_EQ(count_bitmaps, 1u);
}

TEST(HeapGraphTrackerTest, BuildFlamegraph) {
  //           4@A 5@B
  //             \ /
  //         2@Y 3@Y
  //           \ /
  //           1@X

  constexpr uint64_t kSeqId = 1;
  constexpr UniquePid kPid = 1;
  constexpr int64_t kTimestamp = 1;

  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.process_tracker.reset(new ProcessTracker(&context));
  context.process_tracker->GetOrCreateProcess(kPid);

  HeapGraphTracker tracker(context.storage.get());

  constexpr uint64_t kField = 1;
  constexpr uint64_t kLocation = 0;

  constexpr uint64_t kX = 1;
  constexpr uint64_t kY = 2;
  constexpr uint64_t kA = 3;
  constexpr uint64_t kB = 4;

  auto field = base::StringView("foo");
  StringPool::Id x = context.storage->InternString("X");
  StringPool::Id y = context.storage->InternString("Y");
  StringPool::Id a = context.storage->InternString("A");
  StringPool::Id b = context.storage->InternString("B");

  tracker.AddInternedFieldName(kSeqId, kField, field);

  tracker.AddInternedLocationName(kSeqId, kLocation,
                                  context.storage->InternString("location"));
  tracker.AddInternedType(kSeqId, kX, x, kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{}, /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);
  tracker.AddInternedType(kSeqId, kY, y, kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{}, /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);
  tracker.AddInternedType(kSeqId, kA, a, kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{}, /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);
  tracker.AddInternedType(kSeqId, kB, b, kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{}, /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);
  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 1;
    obj.self_size = 1;
    obj.type_id = kX;
    obj.field_name_ids = {kField, kField};
    obj.referred_objects = {2, 3};

    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 2;
    obj.self_size = 2;
    obj.type_id = kY;
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 3;
    obj.self_size = 3;
    obj.type_id = kY;
    obj.field_name_ids = {kField, kField};
    obj.referred_objects = {4, 5};

    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 4;
    obj.self_size = 4;
    obj.type_id = kA;
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 5;
    obj.self_size = 5;
    obj.type_id = kB;
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  HeapGraphTracker::SourceRoot root;
  root.root_type = protos::pbzero::HeapGraphRoot::ROOT_UNKNOWN;
  root.object_ids.emplace_back(1);
  tracker.AddRoot(kSeqId, kPid, kTimestamp, root);

  tracker.FinalizeAllProfiles();
  std::unique_ptr<tables::ExperimentalFlamegraphTable> flame =
      tracker.BuildFlamegraph(kPid, kTimestamp);
  ASSERT_NE(flame, nullptr);

  std::vector<int64_t> cumulative_sizes;
  std::vector<int64_t> cumulative_counts;
  std::vector<int64_t> sizes;
  std::vector<int64_t> counts;
  for (auto it = flame->IterateRows(); it; ++it) {
    cumulative_sizes.push_back(it.cumulative_size());
    cumulative_counts.push_back(it.cumulative_count());
    sizes.push_back(it.size());
    counts.push_back(it.count());
  }

  EXPECT_THAT(cumulative_sizes, UnorderedElementsAre(15, 4, 14, 5));
  EXPECT_THAT(cumulative_counts, UnorderedElementsAre(5, 4, 1, 1));
  EXPECT_THAT(sizes, UnorderedElementsAre(1, 5, 4, 5));
  EXPECT_THAT(counts, UnorderedElementsAre(1, 2, 1, 1));
}

TEST(HeapGraphTrackerTest, BuildFlamegraphWeakReferences) {
  // Regression test for http://b.corp.google.com/issues/302662734:
  // For weak (and other) references, we should not follow the
  // `java.lang.ref.Reference.referent` field, but we should follow other
  // fields.
  //
  //                                   2@A 4@B
  //  (java.lang.ref.Reference.referent) \ / (X.other)
  //                                     1@X (extends WeakReference)

  constexpr uint64_t kSeqId = 1;
  constexpr UniquePid kPid = 1;
  constexpr int64_t kTimestamp = 1;

  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.process_tracker.reset(new ProcessTracker(&context));
  context.process_tracker->GetOrCreateProcess(kPid);

  HeapGraphTracker tracker(context.storage.get());

  constexpr uint64_t kLocation = 0;

  base::StringView referent_field =
      base::StringView("java.lang.ref.Reference.referent");
  constexpr uint64_t kReferentField = 1;
  base::StringView other_field = base::StringView("X.other");
  constexpr uint64_t kOtherField = 2;

  constexpr uint64_t kX = 1;
  StringPool::Id x = context.storage->InternString("X");
  constexpr uint64_t kA = 2;
  StringPool::Id a = context.storage->InternString("A");
  constexpr uint64_t kB = 4;
  StringPool::Id b = context.storage->InternString("B");
  constexpr uint64_t kWeakRef = 5;
  StringPool::Id weak_ref = context.storage->InternString("WeakReference");

  tracker.AddInternedFieldName(kSeqId, kReferentField, referent_field);
  tracker.AddInternedFieldName(kSeqId, kOtherField, other_field);

  tracker.AddInternedLocationName(kSeqId, kLocation,
                                  context.storage->InternString("location"));

  tracker.AddInternedType(kSeqId, kWeakRef, weak_ref, kLocation,
                          /*object_size=*/0,
                          /*field_name_ids=*/{kReferentField},
                          /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_WEAK_REFERENCE);
  tracker.AddInternedType(kSeqId, kX, x, kLocation,
                          /*object_size=*/0,
                          /*field_name_ids=*/{kOtherField},
                          /*superclass_id=*/kWeakRef,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_WEAK_REFERENCE);
  tracker.AddInternedType(kSeqId, kA, a, kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{}, /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);
  tracker.AddInternedType(kSeqId, kB, b, kLocation, /*object_size=*/0,
                          /*field_name_ids=*/{}, /*superclass_id=*/0,
                          /*classloader_id=*/0, /*no_fields=*/false,
                          protos::pbzero::HeapGraphType::KIND_NORMAL);
  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 1;
    obj.self_size = 1;
    obj.type_id = kX;
    obj.referred_objects = {/*X.other*/ 4,
                            /*java.lang.ref.Reference.referent*/ 2};
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 2;
    obj.self_size = 2;
    obj.type_id = kA;
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = 4;
    obj.self_size = 4;
    obj.type_id = kB;
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  }

  HeapGraphTracker::SourceRoot root;
  root.root_type = protos::pbzero::HeapGraphRoot::ROOT_UNKNOWN;
  root.object_ids.emplace_back(1);
  tracker.AddRoot(kSeqId, kPid, kTimestamp, root);

  tracker.FinalizeAllProfiles();
  std::unique_ptr<tables::ExperimentalFlamegraphTable> flame =
      tracker.BuildFlamegraph(kPid, kTimestamp);
  ASSERT_NE(flame, nullptr);

  std::vector<int64_t> cumulative_sizes;
  std::vector<int64_t> cumulative_counts;
  std::vector<int64_t> sizes;
  std::vector<int64_t> counts;
  for (auto it = flame->IterateRows(); it; ++it) {
    cumulative_sizes.push_back(it.cumulative_size());
    cumulative_counts.push_back(it.cumulative_count());
    sizes.push_back(it.size());
    counts.push_back(it.count());
  }

  EXPECT_THAT(cumulative_sizes, UnorderedElementsAre(4, 4 + 1));
  EXPECT_THAT(cumulative_counts, UnorderedElementsAre(1, 1 + 1));
  EXPECT_THAT(sizes, UnorderedElementsAre(1, 4));
  EXPECT_THAT(counts, UnorderedElementsAre(1, 1));
}

constexpr uint64_t kRoot = 1;

class HeapGraphStabilityTest : public ::testing::Test {
 public:
  class Helper {
   public:
    Helper() {
      context_.storage.reset(new TraceStorage());
      context_.process_tracker.reset(new ProcessTracker(&context_));
      context_.process_tracker->GetOrCreateProcess(kPid);
      tracker_ = std::make_unique<HeapGraphTracker>(context_.storage.get());

      tracker_->AddInternedLocationName(
          kSeqId, kLocation, context_.storage->InternString("location"));
      tracker_->AddInternedFieldName(kSeqId, kField, base::StringView("foo"));
    }

    uint64_t GetOrCreateTypeId(const std::string& name) {
      if (auto it = class_name_to_id_.find(name);
          it != class_name_to_id_.end()) {
        return it->second;
      }
      uint64_t id = next_type_id_++;
      class_name_to_id_[name] = id;
      RegisterType(id, name);
      return id;
    }

    void RegisterType(uint64_t id, const std::string& name) {
      tracker_->AddInternedType(
          kSeqId, id, context_.storage->InternString(base::StringView(name)),
          kLocation, 0, {}, 0, 0, false,
          protos::pbzero::HeapGraphType::KIND_NORMAL);
    }

    void RegisterObject(uint64_t id, uint64_t type_id) {
      object_type_ids_[id] = type_id;
    }

    void Link(uint64_t parent_id, uint64_t child_id) {
      if (parent_id == kRoot) {
        roots_.push_back(child_id);
      } else {
        object_refs_[parent_id].push_back(child_id);
      }
    }

    void BuildFlamegraph() {
      for (const auto& [id, type_id] : object_type_ids_) {
        HeapGraphTracker::SourceObject obj;
        obj.object_id = id;
        obj.self_size = 1;  // Default size
        obj.type_id = type_id;

        auto it = object_refs_.find(id);
        if (it != object_refs_.end()) {
          obj.referred_objects = it->second;
          obj.field_name_ids.resize(obj.referred_objects.size(), kField);
        }

        tracker_->AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
      }

      HeapGraphTracker::SourceRoot root;
      root.root_type = protos::pbzero::HeapGraphRoot::ROOT_UNKNOWN;
      for (uint64_t id : roots_) {
        root.object_ids.push_back(id);
      }
      tracker_->AddRoot(kSeqId, kPid, kTimestamp, root);

      tracker_->FinalizeAllProfiles();
      flame_ = tracker_->BuildFlamegraph(kPid, kTimestamp);
      ASSERT_NE(flame_, nullptr);
    }

    void AssertParent(const std::string& child_name,
                      const std::string& expected_parent_name) {
      bool found_child = false;
      for (auto it = flame_->IterateRows(); it; ++it) {
        auto name = context_.storage->string_pool().Get(it.name());
        if (name.ToStdString() == child_name) {
          found_child = true;
          auto parent_id = it.parent_id();
          ASSERT_TRUE(parent_id.has_value());

          std::optional<std::string> parent_name;
          for (auto pit = flame_->IterateRows(); pit; ++pit) {
            if (pit.id().value == parent_id->value) {
              parent_name =
                  context_.storage->string_pool().Get(pit.name()).ToStdString();
              break;
            }
          }
          ASSERT_TRUE(parent_name.has_value());
          EXPECT_THAT(*parent_name, testing::HasSubstr(expected_parent_name));
        }
      }
      EXPECT_TRUE(found_child) << "Child " << child_name << " not found";
    }

    uint64_t root_id = 0;

   private:
    static constexpr uint64_t kSeqId = 1;
    static constexpr UniquePid kPid = 1;
    static constexpr int64_t kTimestamp = 1;
    static constexpr uint64_t kLocation = 0;
    static constexpr uint64_t kField = 1;

    TraceProcessorContext context_;
    std::unique_ptr<HeapGraphTracker> tracker_;
    std::unique_ptr<tables::ExperimentalFlamegraphTable> flame_;

    std::map<uint64_t, uint64_t> object_type_ids_;
    std::map<uint64_t, std::vector<uint64_t>> object_refs_;
    std::vector<uint64_t> roots_;
    std::map<std::string, uint64_t> class_name_to_id_;
    uint64_t next_type_id_ = 1;
  };
};

struct ObjectInfo {
  uint64_t id;
  std::string class_name;
  std::vector<uint64_t> parents;
};

struct ShortestPathTestCase {
  std::string name;
  std::vector<ObjectInfo> objects;

  static std::string ToString(
      const testing::TestParamInfo<ShortestPathTestCase>& info) {
    return info.param.name;
  }
};

class ShortestPathStabilityTest
    : public HeapGraphStabilityTest,
      public testing::WithParamInterface<ShortestPathTestCase> {};

TEST_P(ShortestPathStabilityTest, Run) {
  const auto& test = GetParam();
  Helper helper;

  for (const auto& obj : test.objects) {
    uint64_t type_id = helper.GetOrCreateTypeId(obj.class_name);
    helper.RegisterObject(obj.id, type_id);
    for (uint64_t parent : obj.parents) {
      helper.Link(parent, obj.id);
    }
  }

  helper.BuildFlamegraph();
  helper.AssertParent("Child", "A_Parent");
}

INSTANTIATE_TEST_SUITE_P(
    StabilityCases,
    ShortestPathStabilityTest,
    testing::Values(
        ShortestPathTestCase{
            "Aid_greater_than_Bid__B_references_earlier_A",
            {{/* id= */ 2, /* name= */ "B_Parent", /* parents= */ {kRoot}},
             {/* id= */ 3, /* name= */ "A_Parent", /* parents= */ {kRoot}},
             {/* id= */ 4, /* name= */ "Child", /* parents= */ {2, 3}}}},
        ShortestPathTestCase{
            "Aid_greater_than_Bid__A_references_earlier_B",
            {{/* id= */ 2, /* name= */ "B_Parent", /* parents= */ {kRoot}},
             {/* id= */ 3, /* name= */ "A_Parent", /* parents= */ {kRoot}},
             {/* id= */ 4, /* name= */ "Child", /* parents= */ {3, 2}}}},
        ShortestPathTestCase{
            "Aid_less_than_Bid__B_references_earlier_A",
            {{/* id= */ 2, /* name= */ "A_Parent", /* parents= */ {kRoot}},
             {/* id= */ 3, /* name= */ "B_Parent", /* parents= */ {kRoot}},
             {/* id= */ 4, /* name= */ "Child", /* parents= */ {3, 2}}}},
        ShortestPathTestCase{
            "Aid_less_than_Bid__A_references_earlier_B",
            {{/* id= */ 2, /* name= */ "A_Parent", /* parents= */ {kRoot}},
             {/* id= */ 3, /* name= */ "B_Parent", /* parents= */ {kRoot}},
             {/* id= */ 4, /* name= */ "Child", /* parents= */ {2, 3}}}}),
    &ShortestPathTestCase::ToString);

constexpr char kArray[] = "X[]";
constexpr char kDoubleArray[] = "X[][]";
constexpr char kNoArray[] = "X";
constexpr char kLongNoArray[] = "ABCDE";
constexpr char kStaticClassNoArray[] = "java.lang.Class<abc>";
constexpr char kStaticClassArray[] = "java.lang.Class<abc[]>";

TEST(HeapGraphTrackerTest, NormalizeTypeName) {
  // sizeof(...) - 1 below to get rid of the null-byte.
  EXPECT_EQ(NormalizeTypeName(base::StringView(kArray, sizeof(kArray) - 1))
                .ToStdString(),
            "X");
  EXPECT_EQ(NormalizeTypeName(
                base::StringView(kDoubleArray, sizeof(kDoubleArray) - 1))
                .ToStdString(),
            "X");
  EXPECT_EQ(NormalizeTypeName(base::StringView(kNoArray, sizeof(kNoArray) - 1))
                .ToStdString(),
            "X");
  EXPECT_EQ(NormalizeTypeName(
                base::StringView(kLongNoArray, sizeof(kLongNoArray) - 1))
                .ToStdString(),
            "ABCDE");
  EXPECT_EQ(NormalizeTypeName(base::StringView(kStaticClassNoArray,
                                               sizeof(kStaticClassNoArray) - 1))
                .ToStdString(),
            "abc");
  EXPECT_EQ(NormalizeTypeName(base::StringView(kStaticClassArray,
                                               sizeof(kStaticClassArray) - 1))
                .ToStdString(),
            "abc");
}

TEST(HeapGraphTrackerTest, NumberOfArray) {
  // sizeof(...) - 1 below to get rid of the null-byte.
  EXPECT_EQ(NumberOfArrays(base::StringView(kArray, sizeof(kArray) - 1)), 1u);
  EXPECT_EQ(
      NumberOfArrays(base::StringView(kDoubleArray, sizeof(kDoubleArray) - 1)),
      2u);
  EXPECT_EQ(NumberOfArrays(base::StringView(kNoArray, sizeof(kNoArray) - 1)),
            0u);
  EXPECT_EQ(
      NumberOfArrays(base::StringView(kLongNoArray, sizeof(kLongNoArray) - 1)),
      0u);
}

}  // namespace
}  // namespace perfetto::trace_processor
