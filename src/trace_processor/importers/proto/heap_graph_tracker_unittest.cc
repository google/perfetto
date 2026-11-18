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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "protos/third_party/android/art/heap_graph.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/util/profiler_util.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::com::android::art::tracing::pbzero::HeapGraphRoot;
using ::com::android::art::tracing::pbzero::HeapGraphType;
using ::testing::UnorderedElementsAre;

void InitHeapGraphTestContext(TraceProcessorContext* context, UniquePid pid) {
  context->storage = std::make_unique<TraceStorage>();
  context->machine_tracker =
      std::make_unique<MachineTracker>(context, kDefaultMachineId);
  context->process_tracker = std::make_unique<ProcessTracker>(context);
  context->process_tracker->GetOrCreateProcess(pid);
  context->global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context->storage.get());
  context->trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context->stats_tracker = std::make_unique<StatsTracker>(context);
}

TEST(HeapGraphTrackerTest, PackageFromLocationApp) {
  TraceStorage storage;
  GlobalStatsTracker tracker(&storage);

  const char data_app_path[] =
      "/data/app/org.perfetto.test-6XfQhnaSkFwGK0sYL9is0G==/base.apk";
  EXPECT_EQ(PackageFromLocation(&tracker, data_app_path), "org.perfetto.test");

  const char with_extra_dir[] =
      "/data/app/~~ASDFGH1234QWerT==/"
      "com.perfetto.test-MNBVCX7890SDTst6==/test.apk";
  EXPECT_EQ(PackageFromLocation(&tracker, with_extra_dir), "com.perfetto.test");

  const char odex[] =
      "/data/app/com.google.android.apps.wellbeing-"
      "qfQCaB4uJ7P0OPpZQqOu0Q==/oat/arm64/base.odex";
  EXPECT_EQ(PackageFromLocation(&tracker, odex),
            "com.google.android.apps.wellbeing");

  const char inmem_dex[] =
      "[anon:dalvik-classes.dex extracted in memory from "
      "/data/app/~~uUgHYtbjPNr2VFa3byIF4Q==/"
      "com.perfetto.example-aC94wTfXRC60l2HJU5YvjQ==/base.apk]";
  EXPECT_EQ(PackageFromLocation(&tracker, inmem_dex), "com.perfetto.example");
}

TEST(HeapGraphTrackerTest, PopulateNativeSize) {
  constexpr uint64_t kSeqId = 1;
  constexpr UniquePid kPid = 1;
  constexpr int64_t kTimestamp = 1;

  TraceProcessorContext context;
  InitHeapGraphTestContext(&context, kPid);
  HeapGraphTracker tracker(context.storage.get(),
                           context.global_stats_tracker.get());

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
      ::com::android::art::tracing::pbzero::HeapGraphType::KIND_NORMAL);

  tracker.AddInternedType(
      kSeqId, kTypeCleaner, context.storage->InternString("sun.misc.Cleaner"),
      kLocation, /*object_size=*/0,
      /*field_name_ids=*/{kReferent, kThunk, kNext},
      /*superclass_id=*/0,
      /*classloader_id=*/0, /*no_fields=*/false,
      ::com::android::art::tracing::pbzero::HeapGraphType::KIND_NORMAL);

  tracker.AddInternedType(
      kSeqId, kTypeCleanerThunk,
      context.storage->InternString(
          "libcore.util.NativeAllocationRegistry$CleanerThunk"),
      kLocation, /*object_size=*/0,
      /*field_name_ids=*/{kThis0}, /*superclass_id=*/0,
      /*classloader_id=*/0, /*no_fields=*/false,
      ::com::android::art::tracing::pbzero::HeapGraphType::KIND_NORMAL);

  tracker.AddInternedType(
      kSeqId, kTypeNativeAllocationRegistry,
      context.storage->InternString("libcore.util.NativeAllocationRegistry"),
      kLocation, /*object_size=*/0,
      /*field_name_ids=*/{}, /*superclass_id=*/0,
      /*classloader_id=*/0, /*no_fields=*/false,
      ::com::android::art::tracing::pbzero::HeapGraphType::KIND_NORMAL);

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
    auto class_row = class_table[it.type_id()];
    ASSERT_TRUE(true);
    if (context.storage->string_pool().Get(class_row.name()) ==
        "android.graphics.Bitmap") {
      EXPECT_EQ(it.native_size(), 24242);
      count_bitmaps++;
    } else {
      EXPECT_EQ(it.native_size(), 0)
          << context.storage->string_pool().Get(class_row.name()).c_str()
          << " has non zero native_size";
    }
  }
  EXPECT_EQ(count_bitmaps, 1u);
}

TEST(HeapGraphTrackerTest, MarkReachableObjectsRootDistance) {
  constexpr uint64_t kSeqId = 1;
  constexpr UniquePid kPid = 1;
  constexpr int64_t kTimestamp = 1;

  TraceProcessorContext context;
  InitHeapGraphTestContext(&context, kPid);
  HeapGraphTracker tracker(context.storage.get(),
                           context.global_stats_tracker.get());

  constexpr uint64_t kLocation = 0;
  tracker.AddInternedLocationName(kSeqId, kLocation,
                                  context.storage->InternString("location"));

  enum Fields : uint8_t { kReferent = 1, kStrongField };
  tracker.AddInternedFieldName(kSeqId, kReferent,
                               "java.lang.ref.Reference.referent");
  tracker.AddInternedFieldName(kSeqId, kStrongField, "Foo.strong");

  enum Types : uint8_t {
    kTypeRootA = 1,
    kTypeRootB,
    kTypeMid1,
    kTypeMid2,
    kTypeShared,
    kTypeCycleLeaf,
    kTypeSoftRef,
    kTypeSoftTarget,
    kTypeWeakRef,
    kTypeWeakTarget,
    kTypePhantomRef,
    kTypePhantomTarget,
    kTypeFinalizerRef,
    kTypeFinalizerTarget,
    kTypeUnreachable1,
    kTypeUnreachable2,
  };

  auto add_type = [&](uint64_t id, const char* name,
                      std::vector<uint64_t> fields, HeapGraphType::Kind kind) {
    tracker.AddInternedType(
        kSeqId, id, context.storage->InternString(name), kLocation,
        /*object_size=*/16, std::move(fields), /*superclass_id=*/0,
        /*classloader_id=*/0, /*no_fields=*/false, kind);
  };

  add_type(kTypeRootA, "RootA",
           {kStrongField, kStrongField, kStrongField, kStrongField,
            kStrongField, kStrongField},
           HeapGraphType::KIND_NORMAL);
  add_type(kTypeRootB, "RootB", {kStrongField}, HeapGraphType::KIND_NORMAL);
  add_type(kTypeMid1, "Mid1", {kStrongField}, HeapGraphType::KIND_NORMAL);
  add_type(kTypeMid2, "Mid2", {kStrongField}, HeapGraphType::KIND_NORMAL);
  add_type(kTypeShared, "Shared", {kStrongField}, HeapGraphType::KIND_NORMAL);
  add_type(kTypeCycleLeaf, "CycleLeaf", {kStrongField},
           HeapGraphType::KIND_NORMAL);
  add_type(kTypeSoftRef, "SoftRef", {kReferent},
           HeapGraphType::KIND_SOFT_REFERENCE);
  add_type(kTypeSoftTarget, "SoftTarget", {}, HeapGraphType::KIND_NORMAL);
  add_type(kTypeWeakRef, "WeakRef", {kReferent},
           HeapGraphType::KIND_WEAK_REFERENCE);
  add_type(kTypeWeakTarget, "WeakTarget", {}, HeapGraphType::KIND_NORMAL);
  add_type(kTypePhantomRef, "PhantomRef", {kReferent},
           HeapGraphType::KIND_PHANTOM_REFERENCE);
  add_type(kTypePhantomTarget, "PhantomTarget", {}, HeapGraphType::KIND_NORMAL);
  add_type(kTypeFinalizerRef, "FinalizerRef", {kReferent},
           HeapGraphType::KIND_FINALIZER_REFERENCE);
  add_type(kTypeFinalizerTarget, "FinalizerTarget", {},
           HeapGraphType::KIND_NORMAL);
  add_type(kTypeUnreachable1, "Unreachable1", {kStrongField},
           HeapGraphType::KIND_NORMAL);
  add_type(kTypeUnreachable2, "Unreachable2", {kStrongField},
           HeapGraphType::KIND_NORMAL);

  enum Objects : uint8_t {
    kObjRootA = 1,
    kObjRootB,
    kObjMid1,
    kObjMid2,
    kObjShared,
    kObjCycleLeaf,
    kObjSoftRef,
    kObjSoftTarget,
    kObjWeakRef,
    kObjWeakTarget,
    kObjPhantomRef,
    kObjPhantomTarget,
    kObjFinalizerRef,
    kObjFinalizerTarget,
    kObjUnreachable1,
    kObjUnreachable2,
  };

  auto add_obj = [&](uint64_t id, uint64_t type_id,
                     std::vector<uint64_t> refs) {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = id;
    obj.type_id = type_id;
    obj.self_size = 16;
    obj.referred_objects = std::move(refs);
    tracker.AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
  };

  // RootA reaches RootB directly (RootB must remain at distance 0), starts a
  // longer path to Shared (RootA -> Mid1 -> Mid2 -> Shared), and holds soft,
  // weak, phantom, and finalizer references.
  add_obj(kObjRootA, kTypeRootA,
          {kObjRootB, kObjMid1, kObjSoftRef, kObjWeakRef, kObjPhantomRef,
           kObjFinalizerRef});
  add_obj(kObjRootB, kTypeRootB, {kObjShared});
  add_obj(kObjMid1, kTypeMid1, {kObjMid2});
  add_obj(kObjMid2, kTypeMid2, {kObjShared});
  add_obj(kObjShared, kTypeShared, {kObjCycleLeaf});
  // Cycle back into Mid1; Mid1 must retain its shortest distance of 1.
  add_obj(kObjCycleLeaf, kTypeCycleLeaf, {kObjMid1});
  add_obj(kObjSoftRef, kTypeSoftRef, {kObjSoftTarget});
  add_obj(kObjSoftTarget, kTypeSoftTarget, {});
  add_obj(kObjWeakRef, kTypeWeakRef, {kObjWeakTarget});
  add_obj(kObjWeakTarget, kTypeWeakTarget, {});
  add_obj(kObjPhantomRef, kTypePhantomRef, {kObjPhantomTarget});
  add_obj(kObjPhantomTarget, kTypePhantomTarget, {});
  add_obj(kObjFinalizerRef, kTypeFinalizerRef, {kObjFinalizerTarget});
  add_obj(kObjFinalizerTarget, kTypeFinalizerTarget, {});
  // Unreachable 2-cycle.
  add_obj(kObjUnreachable1, kTypeUnreachable1, {kObjUnreachable2});
  add_obj(kObjUnreachable2, kTypeUnreachable2, {kObjUnreachable1});

  // Duplicate RootA with multiple root types to verify RankRoot precedence:
  // ROOT_STICKY_CLASS > ROOT_JNI_GLOBAL > ROOT_JNI_LOCAL.
  tracker.AddRoot(kSeqId, kPid, kTimestamp,
                  {HeapGraphRoot::ROOT_JNI_LOCAL, {kObjRootA}});
  tracker.AddRoot(kSeqId, kPid, kTimestamp,
                  {HeapGraphRoot::ROOT_STICKY_CLASS, {kObjRootA}});
  tracker.AddRoot(kSeqId, kPid, kTimestamp,
                  {HeapGraphRoot::ROOT_JNI_GLOBAL, {kObjRootA, kObjRootB}});

  tracker.FinalizeProfile(kSeqId);

  struct ExpectedState {
    bool reachable;
    std::optional<std::string> root_type;
    int32_t root_distance;
  };
  const std::map<std::string, ExpectedState> expected = {
      {"RootA", {true, "ROOT_STICKY_CLASS", 0}},
      {"RootB", {true, "ROOT_JNI_GLOBAL", 0}},
      {"Mid1", {true, std::nullopt, 1}},
      {"Mid2", {true, std::nullopt, 2}},
      {"Shared", {true, std::nullopt, 1}},
      {"CycleLeaf", {true, std::nullopt, 2}},
      {"SoftRef", {true, std::nullopt, 1}},
      {"SoftTarget", {true, std::nullopt, 2}},
      {"WeakRef", {true, std::nullopt, 1}},
      {"WeakTarget", {false, std::nullopt, -1}},
      {"PhantomRef", {true, std::nullopt, 1}},
      {"PhantomTarget", {false, std::nullopt, -1}},
      {"FinalizerRef", {true, std::nullopt, 1}},
      {"FinalizerTarget", {false, std::nullopt, -1}},
      {"Unreachable1", {false, std::nullopt, -1}},
      {"Unreachable2", {false, std::nullopt, -1}},
  };

  const auto& objs_table = context.storage->heap_graph_object_table();
  const auto& class_table = context.storage->heap_graph_class_table();
  ASSERT_EQ(objs_table.row_count(), expected.size());
  for (auto it = objs_table.IterateRows(); it; ++it) {
    std::string cls_name =
        context.storage->GetString(class_table[it.type_id()].name())
            .ToStdString();
    auto exp_it = expected.find(cls_name);
    ASSERT_NE(exp_it, expected.end()) << "Unexpected class: " << cls_name;
    EXPECT_EQ(it.reachable() != 0, exp_it->second.reachable) << cls_name;
    std::optional<std::string> actual_root_type;
    if (it.root_type().has_value()) {
      actual_root_type =
          context.storage->GetString(*it.root_type()).ToStdString();
    }
    EXPECT_EQ(actual_root_type, exp_it->second.root_type) << cls_name;
    EXPECT_EQ(it.root_distance(), exp_it->second.root_distance) << cls_name;
  }
}

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
