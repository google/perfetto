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

#include "perfetto/ext/base/string_view.h"
#include "protos/perfetto/trace/profiling/heap_graph.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/util/profiler_util.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
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
  context.storage = std::make_shared<TraceStorage>();
  context.process_tracker = std::make_unique<ProcessTracker>(&context);
  context.process_tracker->GetOrCreateProcess(kPid);

  HeapGraphTracker tracker(context.storage.get());

  constexpr uint64_t kLocation = 0;
  tracker.AddInternedLocationName(kSeqId, kLocation,
                                  context.storage->InternString("location"));

  enum Fields : uint64_t { kReferent = 1, kThunk, kThis0, kNext };

  tracker.AddInternedFieldName(kSeqId, kReferent,
                               "java.lang.ref.Reference.referent");
  tracker.AddInternedFieldName(kSeqId, kThunk, "sun.misc.Cleaner.thunk");
  tracker.AddInternedFieldName(
      kSeqId, kThis0,
      "libcore.util.NativeAllocationRegistry$CleanerThunk.this$0");
  tracker.AddInternedFieldName(kSeqId, kNext, "sun.misc.Cleaner.next");

  enum Types : uint64_t {
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

  enum Objects : uint64_t {
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

  tracker.FinalizeProfile(kSeqId);
  std::unique_ptr<tables::ExperimentalFlamegraphTable> flame =
      tracker.BuildFlamegraph(kPid, kTimestamp);
  ASSERT_NE(flame, nullptr);

  auto cumulative_sizes = flame->cumulative_size().ToVectorForTesting();
  EXPECT_THAT(cumulative_sizes, UnorderedElementsAre(15, 4, 14, 5));

  auto cumulative_counts = flame->cumulative_count().ToVectorForTesting();
  EXPECT_THAT(cumulative_counts, UnorderedElementsAre(5, 4, 1, 1));

  auto sizes = flame->size().ToVectorForTesting();
  EXPECT_THAT(sizes, UnorderedElementsAre(1, 5, 4, 5));

  auto counts = flame->count().ToVectorForTesting();
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

  tracker.FinalizeProfile(kSeqId);
  std::unique_ptr<tables::ExperimentalFlamegraphTable> flame =
      tracker.BuildFlamegraph(kPid, kTimestamp);
  ASSERT_NE(flame, nullptr);

  auto cumulative_sizes = flame->cumulative_size().ToVectorForTesting();
  EXPECT_THAT(cumulative_sizes, UnorderedElementsAre(4, 4 + 1));

  auto cumulative_counts = flame->cumulative_count().ToVectorForTesting();
  EXPECT_THAT(cumulative_counts, UnorderedElementsAre(1, 1 + 1));

  auto sizes = flame->size().ToVectorForTesting();
  EXPECT_THAT(sizes, UnorderedElementsAre(1, 4));

  auto counts = flame->count().ToVectorForTesting();
  EXPECT_THAT(counts, UnorderedElementsAre(1, 1));
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
}  // namespace trace_processor
}  // namespace perfetto
