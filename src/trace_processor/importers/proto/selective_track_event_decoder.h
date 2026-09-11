/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_SELECTIVE_TRACK_EVENT_DECODER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_SELECTIVE_TRACK_EVENT_DECODER_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/importers/proto/typed_proto_field.h"

namespace perfetto::trace_processor {

// The fields handed to TrackEvent extension plugins are TrackEvent fields.
using TrackEventField = TypedProtoField;

namespace internal {

// The TrackEvent fields that describe the event itself, read through the
// generated accessors. Every other field is imported as args: the in-tree
// arg fields such as task_execution and the chrome typed messages, and the
// out-of-tree extensions (`extensions 1000 to 9999`). Those land in
// unknown_fields(), in wire order, which drives arg field dispatch.
using TrackEventCoreMask = protozero::SelectiveDecodeMask<
    protos::pbzero::TrackEvent::kTimestampDeltaUsFieldNumber,
    protos::pbzero::TrackEvent::kThreadTimeDeltaUsFieldNumber,
    protos::pbzero::TrackEvent::kCategoryIidsFieldNumber,
    protos::pbzero::TrackEvent::kDebugAnnotationsFieldNumber,
    protos::pbzero::TrackEvent::kLegacyEventFieldNumber,
    protos::pbzero::TrackEvent::kThreadInstructionCountDeltaFieldNumber,
    protos::pbzero::TrackEvent::kTypeFieldNumber,
    protos::pbzero::TrackEvent::kNameIidFieldNumber,
    protos::pbzero::TrackEvent::kTrackUuidFieldNumber,
    protos::pbzero::TrackEvent::kExtraCounterValuesFieldNumber,
    protos::pbzero::TrackEvent::kTimestampAbsoluteUsFieldNumber,
    protos::pbzero::TrackEvent::kThreadTimeAbsoluteUsFieldNumber,
    protos::pbzero::TrackEvent::kThreadInstructionCountAbsoluteFieldNumber,
    protos::pbzero::TrackEvent::kCategoriesFieldNumber,
    protos::pbzero::TrackEvent::kNameFieldNumber,
    protos::pbzero::TrackEvent::kCounterValueFieldNumber,
    protos::pbzero::TrackEvent::kExtraCounterTrackUuidsFieldNumber,
    protos::pbzero::TrackEvent::kFlowIdsOldFieldNumber,
    protos::pbzero::TrackEvent::kTerminatingFlowIdsOldFieldNumber,
    protos::pbzero::TrackEvent::kDoubleCounterValueFieldNumber,
    protos::pbzero::TrackEvent::kExtraDoubleCounterTrackUuidsFieldNumber,
    protos::pbzero::TrackEvent::kExtraDoubleCounterValuesFieldNumber,
    protos::pbzero::TrackEvent::kFlowIdsFieldNumber,
    protos::pbzero::TrackEvent::kTerminatingFlowIdsFieldNumber,
    protos::pbzero::TrackEvent::kCorrelationIdFieldNumber,
    protos::pbzero::TrackEvent::kCorrelationIdStrFieldNumber,
    protos::pbzero::TrackEvent::kCorrelationIdStrIidFieldNumber,
    protos::pbzero::TrackEvent::kCallstackFieldNumber,
    protos::pbzero::TrackEvent::kCallstackIidFieldNumber,
    protos::pbzero::TrackEvent::kCallstackWeightFieldNumber>;

inline constexpr TrackEventCoreMask kTrackEventCoreMask{};

}  // namespace internal

// The generated TrackEvent decoder, decoded selectively: the fields of the
// event itself are available through the generated accessors, every other
// field is in unknown_fields(). One decode serves both the row and the arg
// field dispatch.
class SelectiveTrackEventDecoder : public protozero::SelectiveTypedProtoDecoder<
                                       protos::pbzero::TrackEvent::Decoder> {
 public:
  SelectiveTrackEventDecoder(const uint8_t* data, size_t length)
      : SelectiveTypedProtoDecoder(data,
                                   length,
                                   internal::kTrackEventCoreMask) {}
  explicit SelectiveTrackEventDecoder(protozero::ConstBytes blob)
      : SelectiveTrackEventDecoder(blob.data, blob.size) {}
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_SELECTIVE_TRACK_EVENT_DECODER_H_
