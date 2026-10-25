/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/plugins/winscope_importer/viewcapture_deinterner.h"

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/string_view.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_interned_data.pbzero.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/stats.h"

namespace perfetto::trace_processor::winscope {

namespace {
using ViewCaptureRow = tables::ViewCaptureTable::RowReference;
using ViewRow = tables::ViewCaptureViewTable::RowReference;
}  // namespace

ViewCaptureDeinterner::ViewCaptureDeinterner(
    TraceProcessorContext& context,
    PacketSequenceStateGeneration* sequence_state,
    ViewCaptureRow* snapshot_row,
    ViewRow* view_row)
    : context_(context),
      sequence_state_(sequence_state),
      snapshot_row_(snapshot_row),
      view_row_(view_row) {}

std::optional<base::StringView> ViewCaptureDeinterner::Lookup(ArgKey key,
                                                              uint64_t iid) {
  auto value = TryDeinternString(context_.storage->GetString(key.key), iid);
  if (!value) {
    context_.stats_tracker->IncrementStats(
        stats::winscope_viewcapture_missing_interned_string_parse_errors);
    return std::nullopt;
  }
  flat_key_to_iid_args[key.flat_key].Insert(
      iid, context_.storage->InternString(*value));
  return value;
}

std::optional<base::StringView> ViewCaptureDeinterner::TryDeinternString(
    base::StringView key,
    uint64_t iid) {
  using com::android::internal::pbzero::FrameworksBaseInternedData;
  if (key.EndsWith("class_name_iid")) {
    return DeinternString<
        FrameworksBaseInternedData::kViewcaptureClassNameFieldNumber>(
        iid, view_row_, &ViewRow::set_class_name);
  }
  if (key.EndsWith("package_name_iid")) {
    return DeinternString<
        FrameworksBaseInternedData::kViewcapturePackageNameFieldNumber>(
        iid, snapshot_row_, &ViewCaptureRow::set_package_name);
  }
  if (key.EndsWith("view_id_iid")) {
    return DeinternString<
        FrameworksBaseInternedData::kViewcaptureViewIdFieldNumber>(
        iid, view_row_, &ViewRow::set_view_id);
  }
  if (key.EndsWith("window_name_iid")) {
    return DeinternString<
        FrameworksBaseInternedData::kViewcaptureWindowNameFieldNumber>(
        iid, snapshot_row_, &ViewCaptureRow::set_window_name);
  }
  if (key.EndsWith("content_description_iid")) {
    return DeinternString<
        FrameworksBaseInternedData::kViewcaptureContentDescriptionFieldNumber>(
        iid);
  }
  if (key.EndsWith("text_iid")) {
    return DeinternString<
        FrameworksBaseInternedData::kViewcaptureTextFieldNumber>(iid);
  }
  return std::nullopt;
}

template <uint32_t FieldNumber, typename RowRef>
std::optional<base::StringView> ViewCaptureDeinterner::DeinternString(
    uint64_t iid,
    RowRef* row,
    void (RowRef::*setter)(StringPool::Id)) {
  auto sv = DeinternString<FieldNumber>(iid);
  if (sv && row) {
    (row->*setter)(context_.storage->mutable_string_pool()->InternString(*sv));
  }
  return sv;
}

template <uint32_t FieldNumber>
std::optional<base::StringView> ViewCaptureDeinterner::DeinternString(
    uint64_t iid) {
  return sequence_state_->InternedStringView(FieldNumber, iid);
}

}  // namespace perfetto::trace_processor::winscope
