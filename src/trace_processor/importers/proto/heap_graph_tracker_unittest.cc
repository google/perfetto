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

using ::testing::UnorderedElementsAre;

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

struct NativeSizeTestEnv {
  static constexpr uint64_t kSeqId = 1;
  static constexpr UniquePid kPid = 1;
  static constexpr int64_t kTimestamp = 1;
  static constexpr uint64_t kLocation = 0;

  enum Fields : uint8_t { kReferent = 1, kThunk, kThis0, kNext };
  enum Types : uint8_t {
    kTypeReferent = 1,
    kTypeCleaner,
    kTypeCleanerThunk,
    kTypeNativeAllocationRegistry,
  };

  TraceProcessorContext context;
  std::unique_ptr<HeapGraphTracker> tracker;

  explicit NativeSizeTestEnv(const char* referent_class_name) {
    context.storage = std::make_unique<TraceStorage>();
    context.machine_tracker =
        std::make_unique<MachineTracker>(&context, kDefaultMachineId);
    context.process_tracker = std::make_unique<ProcessTracker>(&context);
    context.process_tracker->GetOrCreateProcess(kPid);
    context.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context.storage.get());
    context.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context.stats_tracker = std::make_unique<StatsTracker>(&context);
    tracker = std::make_unique<HeapGraphTracker>(
        context.storage.get(), context.global_stats_tracker.get());

    tracker->AddInternedLocationName(kSeqId, kLocation,
                                     context.storage->InternString("location"));
    tracker->AddInternedFieldName(kSeqId, kReferent,
                                  "java.lang.ref.Reference.referent");
    tracker->AddInternedFieldName(kSeqId, kThunk, "sun.misc.Cleaner.thunk");
    tracker->AddInternedFieldName(
        kSeqId, kThis0,
        "libcore.util.NativeAllocationRegistry$CleanerThunk.this$0");
    tracker->AddInternedFieldName(kSeqId, kNext, "sun.misc.Cleaner.next");

    using Kind = ::com::android::art::tracing::pbzero::HeapGraphType;
    tracker->AddInternedType(
        kSeqId, kTypeReferent,
        context.storage->InternString(referent_class_name), kLocation,
        /*object_size=*/0, /*field_name_ids=*/{}, /*superclass_id=*/0,
        /*classloader_id=*/0, /*no_fields=*/false, Kind::KIND_NORMAL);
    tracker->AddInternedType(
        kSeqId, kTypeCleaner, context.storage->InternString("sun.misc.Cleaner"),
        kLocation,
        /*object_size=*/0, /*field_name_ids=*/{kReferent, kThunk, kNext},
        /*superclass_id=*/0, /*classloader_id=*/0, /*no_fields=*/false,
        Kind::KIND_NORMAL);
    tracker->AddInternedType(
        kSeqId, kTypeCleanerThunk,
        context.storage->InternString(
            "libcore.util.NativeAllocationRegistry$CleanerThunk"),
        kLocation, /*object_size=*/0, /*field_name_ids=*/{kThis0},
        /*superclass_id=*/0, /*classloader_id=*/0, /*no_fields=*/false,
        Kind::KIND_NORMAL);
    tracker->AddInternedType(
        kSeqId, kTypeNativeAllocationRegistry,
        context.storage->InternString("libcore.util.NativeAllocationRegistry"),
        kLocation, /*object_size=*/0, /*field_name_ids=*/{},
        /*superclass_id=*/0, /*classloader_id=*/0, /*no_fields=*/false,
        Kind::KIND_NORMAL);
  }

  void AddRegisteredObject(uint64_t referent_id,
                           uint64_t cleaner_id,
                           uint64_t thunk_id,
                           uint64_t nar_id,
                           std::optional<int64_t> dynamic_size = std::nullopt) {
    {
      HeapGraphTracker::SourceObject obj;
      obj.object_id = referent_id;
      obj.type_id = kTypeReferent;
      tracker->AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
    }
    {
      HeapGraphTracker::SourceObject obj;
      obj.object_id = cleaner_id;
      obj.type_id = kTypeCleaner;
      obj.referred_objects = {referent_id, thunk_id, 0};
      tracker->AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
    }
    {
      HeapGraphTracker::SourceObject obj;
      obj.object_id = thunk_id;
      obj.type_id = kTypeCleanerThunk;
      obj.referred_objects = {nar_id};
      obj.cleaner_thunk_dynamic_native_size = dynamic_size;
      tracker->AddObject(kSeqId, kPid, kTimestamp, std::move(obj));
    }
  }
};

TEST(HeapGraphTrackerTest, PopulateNativeSize) {
  NativeSizeTestEnv env("android.graphics.Bitmap");
  enum Objects : uint8_t {
    kObjBitmap = 1,
    kObjCleaner,
    kObjThunk,
    kObjNativeAllocationRegistry,
  };

  env.AddRegisteredObject(kObjBitmap, kObjCleaner, kObjThunk,
                          kObjNativeAllocationRegistry);
  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = kObjNativeAllocationRegistry;
    obj.type_id = NativeSizeTestEnv::kTypeNativeAllocationRegistry;
    // NativeAllocationRegistry.size least significant bit is used to encode the
    // source of the allocation (1: malloc, 0: other).
    obj.native_allocation_registry_size = 24242 | 1;
    env.tracker->AddObject(NativeSizeTestEnv::kSeqId, NativeSizeTestEnv::kPid,
                           NativeSizeTestEnv::kTimestamp, std::move(obj));
  }

  env.tracker->FinalizeProfile(NativeSizeTestEnv::kSeqId);

  const auto& objs_table = env.context.storage->heap_graph_object_table();
  const auto& class_table = env.context.storage->heap_graph_class_table();
  size_t count_bitmaps = 0;
  for (auto it = objs_table.IterateRows(); it; ++it) {
    auto class_row = class_table[it.type_id()];
    if (env.context.storage->string_pool().Get(class_row.name()) ==
        "android.graphics.Bitmap") {
      EXPECT_EQ(it.native_size(), 24242);
      count_bitmaps++;
    } else {
      EXPECT_EQ(it.native_size(), 0)
          << env.context.storage->string_pool().Get(class_row.name()).c_str()
          << " has non zero native_size";
    }
  }
  EXPECT_EQ(count_bitmaps, 1u);
}

TEST(HeapGraphTrackerTest, PopulateDynamicNativeSize) {
  NativeSizeTestEnv env("android.os.Parcel");
  enum Objects : uint8_t {
    kObjNativeAllocationRegistry = 1,
    kObjParcelDynamicOdd,
    kObjCleanerDynamicOdd,
    kObjThunkDynamicOdd,
    kObjParcelDynamicZero,
    kObjCleanerDynamicZero,
    kObjThunkDynamicZero,
    kObjParcelFallbackStatic,
    kObjCleanerFallbackStatic,
    kObjThunkFallbackStatic,
  };

  {
    HeapGraphTracker::SourceObject obj;
    obj.object_id = kObjNativeAllocationRegistry;
    obj.type_id = NativeSizeTestEnv::kTypeNativeAllocationRegistry;
    obj.native_allocation_registry_size = 24242 | 1;
    env.tracker->AddObject(NativeSizeTestEnv::kSeqId, NativeSizeTestEnv::kPid,
                           NativeSizeTestEnv::kTimestamp, std::move(obj));
  }
  // 1. Dynamic native size is a raw byte count (bit 0 is not an IS_MALLOCED
  //    flag, so odd values like 12345 are preserved).
  env.AddRegisteredObject(kObjParcelDynamicOdd, kObjCleanerDynamicOdd,
                          kObjThunkDynamicOdd, kObjNativeAllocationRegistry,
                          /*dynamic_size=*/12345);
  // 2. A dynamic size of 0 overrides the registry's non-zero static size.
  env.AddRegisteredObject(kObjParcelDynamicZero, kObjCleanerDynamicZero,
                          kObjThunkDynamicZero, kObjNativeAllocationRegistry,
                          /*dynamic_size=*/0);
  // 3. A thunk without a dynamic size on the same registry falls back to the
  //    registry's static size (24242).
  env.AddRegisteredObject(kObjParcelFallbackStatic, kObjCleanerFallbackStatic,
                          kObjThunkFallbackStatic, kObjNativeAllocationRegistry,
                          /*dynamic_size=*/std::nullopt);

  env.tracker->FinalizeProfile(NativeSizeTestEnv::kSeqId);

  const auto& objs_table = env.context.storage->heap_graph_object_table();
  const auto& class_table = env.context.storage->heap_graph_class_table();
  std::vector<int64_t> parcel_sizes;
  for (auto it = objs_table.IterateRows(); it; ++it) {
    auto class_row = class_table[it.type_id()];
    if (env.context.storage->string_pool().Get(class_row.name()) ==
        "android.os.Parcel") {
      parcel_sizes.push_back(it.native_size());
    } else {
      EXPECT_EQ(it.native_size(), 0)
          << env.context.storage->string_pool().Get(class_row.name()).c_str()
          << " has non zero native_size";
    }
  }
  EXPECT_THAT(parcel_sizes, UnorderedElementsAre(12345, 0, 24242));
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
