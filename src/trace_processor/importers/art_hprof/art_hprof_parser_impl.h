/*
 * Copyright (C) 2025 The Android Open Source Project
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
#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_PARSER_IMPL_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_PARSER_IMPL_H_

#include <cstdint>
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::art_hprof {

class ArtHprofParserImpl : public ArtHprofParser {
 public:
  explicit ArtHprofParserImpl(TraceProcessorContext*);
  ~ArtHprofParserImpl() override;
  void ParseArtHprofEvent(int64_t ts, ArtHprofEvent) override;

 private:
  void PopulateClasses(const HeapGraph& graph,
                      std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>& class_map);

  void PopulateObjects(const HeapGraph& graph,
                      int64_t ts,
                      UniquePid upid,
                      const std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>& class_map,
                      std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>& object_map);

  void PopulateReferences(const HeapGraph& graph,
                         const std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>& object_map);

  std::string GetFieldTypeName(FieldType type);

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor::art_hprof
#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_PARSER_IMPL_H_
