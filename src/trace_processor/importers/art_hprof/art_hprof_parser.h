/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_hprof/art_heap_graph.h"
#include "src/trace_processor/importers/art_hprof/art_heap_graph_builder.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::art_hprof {

constexpr const char* kJavaLangObject = "java.lang.Object";
constexpr const char* kUnknownClassKind = "[unknown class kind]";
constexpr size_t kRecordLengthOffset = 5;
// Full record header: tag(1) + time(4) + length(4) = 9 bytes.
constexpr size_t kRecordHeaderSize = 9;

class ArtHprofParser : public ChunkedTraceReader {
 public:
  explicit ArtHprofParser(TraceProcessorContext* context);
  ~ArtHprofParser() override;
  base::Status Parse(TraceBlobView blob) override;
  base::Status OnPushDataToSorter() override;
  void OnEventsFullyExtracted() override {}

 private:
  void PopulateClasses(const HeapGraph& graph);
  void PopulateObjects(const HeapGraph& graph, int64_t ts, UniquePid upid);
  void PopulateReferences(const HeapGraph& graph);
  void PopulateFieldValues(const HeapGraph& graph);
  // Writes the primitive field values of `obj` and returns how many rows were
  // inserted.
  uint32_t InsertPrimitiveFields(const HeapGraph& graph,
                                 const Object& obj,
                                 uint32_t field_set_id,
                                 tables::HeapGraphPrimitiveTable& prim_table);
  void InsertArrayData(const Object& obj,
                       tables::HeapGraphObjectDataTable::Row& data_row);

  tables::HeapGraphClassTable::Id* FindClassId(uint64_t class_id) const;
  tables::HeapGraphClassTable::Id* FindClassObjectId(uint64_t obj_id) const;
  StringId InternClassName(const std::string& class_name);

  class TraceBlobViewIterator : public ByteIterator {
   public:
    explicit TraceBlobViewIterator();
    ~TraceBlobViewIterator() override;
    bool ReadU1(uint8_t& value) override;
    bool ReadU2(uint16_t& value) override;
    bool ReadU4(uint32_t& value) override;
    bool ReadId(uint64_t& value, uint32_t id_size) override;
    bool ReadString(std::string& str, size_t length) override;
    bool ReadInto(uint8_t* dst, size_t length) override;
    bool ReadView(TraceBlobView& view, size_t length) override;
    bool SkipBytes(size_t count) override;
    size_t GetPosition() const override;
    // Whether we can read an entire record from the existing chunk.
    // This method does not advance the iterator.
    bool CanReadRecord() const override;
    // Pushes more HPROF chunks in for parsin.
    void PushBlob(TraceBlobView blob) override;
    // Shrinks the backing HPROF data to discard all consumed bytes.
    void Shrink() override;

   private:
    // Returns a pointer to the next `length` bytes without advancing, using
    // the scratch buffer only when they straddle two chunks.
    const uint8_t* Peek(size_t length);

    util::TraceBlobViewReader reader_;
    size_t current_offset_ = 0;
    uint8_t scratch_[8];
  };

  TraceProcessorContext* const context_;

  std::unique_ptr<ByteIterator> byte_iterator_;
  std::unique_ptr<HeapGraphBuilder> parser_;

  // HPROF ID → table row ID mappings, used during PopulateObjects/References.
  base::FlatHashMap<uint64_t, tables::HeapGraphClassTable::Id> class_map_;
  base::FlatHashMap<uint64_t, tables::HeapGraphClassTable::Id>
      class_object_map_;
  // Row id in the object table for each object in the heap graph, indexed by
  // ObjectIndex. kInvalidRow for objects which were not inserted.
  static constexpr uint32_t kInvalidRow = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> object_rows_;
  base::FlatHashMap<uint64_t, std::string> class_name_map_;
};
}  // namespace perfetto::trace_processor::art_hprof
#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_PARSER_H_
