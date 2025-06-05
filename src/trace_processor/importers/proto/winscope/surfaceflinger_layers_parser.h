/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_PARSER_H_

#include "protos/perfetto/trace/android/surfaceflinger_layers.pbzero.h"
#include "src/trace_processor/importers/proto/args_parser.h"
#include "src/trace_processor/importers/proto/winscope/surfaceflinger_rect_computation.h"
#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

namespace perfetto {

namespace trace_processor {

class TraceProcessorContext;

class SurfaceFlingerLayersParser {
 public:
  explicit SurfaceFlingerLayersParser(TraceProcessorContext*);
  void Parse(int64_t timestamp,
             protozero::ConstBytes decoder,
             std::optional<uint32_t> sequence_id);

 private:
  const tables::SurfaceFlingerLayersSnapshotTable::Id ParseSnapshot(
      int64_t timestamp,
      protozero::ConstBytes blob,
      std::optional<uint32_t> sequence_id);

  void ParseLayer(
      int64_t timestamp,
      protozero::ConstBytes blob,
      tables::SurfaceFlingerLayersSnapshotTable::Id snapshot_id,
      const std::optional<VisibilityProperties>& visibility,
      const std::unordered_map<int, protozero::ConstBytes>& layers_by_id,
      const SfRectComputation::SurfaceFlingerRects& rects);
  tables::SurfaceFlingerLayerTable::Id InsertLayerRow(
      protozero::ConstBytes blob,
      tables::SurfaceFlingerLayersSnapshotTable::Id snapshot_id,
      const std::optional<VisibilityProperties>& visibility,
      const std::unordered_map<int, protozero::ConstBytes>& layers_by_id,
      const SfRectComputation::SurfaceFlingerRects& rects);
  void TryAddBlockingLayerArgs(const std::unordered_set<int>& blocking_layers,
                               const std::string key_prefix,
                               ArgsParser& writer);

  void ParseDisplay(
      const protos::pbzero::DisplayProto::Decoder& display_decoder,
      tables::SurfaceFlingerLayersSnapshotTable::Id snapshot_id,
      int index,
      std::unordered_map<uint32_t, WinscopeRect>& displays_by_layer_stack);
  tables::WinscopeRectTable::Id* InsertDisplayRectRow(
      const protos::pbzero::DisplayProto::Decoder& display_decoder,
      std::unordered_map<uint32_t, WinscopeRect>& displays_by_layer_stack);
  tables::WinscopeTraceRectTable::Id InsertDisplayTraceRectRow(
      const protos::pbzero::DisplayProto::Decoder& display_decoder,
      const tables::WinscopeRectTable::Id& rect_id,
      int index);

  TraceProcessorContext* const context_;
  util::ProtoToArgsParser args_parser_;

  const uint32_t INVALID_LAYER_STACK = 4294967295;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_SURFACEFLINGER_LAYERS_PARSER_H_
