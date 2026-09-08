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

#include "src/trace_processor/importers/proto/track_event_arg_fields.h"

#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/gpu_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/active_chrome_processes_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/common/android_log_constants.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_track_event.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_active_processes.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_histogram_sample.pbzero.h"
#include "protos/perfetto/trace/track_event/log_message.pbzero.h"
#include "protos/perfetto/trace/track_event/screenshot.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "protos/perfetto/trace/track_event/task_execution.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using TrackEvent = protos::pbzero::TrackEvent;

protos::pbzero::AndroidLogPriority ToAndroidLogPriority(
    protos::pbzero::LogMessage::Priority prio) {
  switch (prio) {
    case protos::pbzero::LogMessage::Priority::PRIO_UNSPECIFIED:
      return protos::pbzero::AndroidLogPriority::PRIO_UNSPECIFIED;
    case protos::pbzero::LogMessage::Priority::PRIO_UNUSED:
      return protos::pbzero::AndroidLogPriority::PRIO_UNUSED;
    case protos::pbzero::LogMessage::Priority::PRIO_VERBOSE:
      return protos::pbzero::AndroidLogPriority::PRIO_VERBOSE;
    case protos::pbzero::LogMessage::Priority::PRIO_DEBUG:
      return protos::pbzero::AndroidLogPriority::PRIO_DEBUG;
    case protos::pbzero::LogMessage::Priority::PRIO_INFO:
      return protos::pbzero::AndroidLogPriority::PRIO_INFO;
    case protos::pbzero::LogMessage::Priority::PRIO_WARN:
      return protos::pbzero::AndroidLogPriority::PRIO_WARN;
    case protos::pbzero::LogMessage::Priority::PRIO_ERROR:
      return protos::pbzero::AndroidLogPriority::PRIO_ERROR;
    case protos::pbzero::LogMessage::Priority::PRIO_FATAL:
      return protos::pbzero::AndroidLogPriority::PRIO_FATAL;
  }
  return protos::pbzero::AndroidLogPriority::PRIO_UNSPECIFIED;
}

}  // namespace

TrackEventArgFieldParser::TrackEventArgFieldParser(
    TrackEventExtensionParserContext* extension_context,
    TraceProcessorContext* context,
    ActiveChromeProcessesTracker* active_processes_tracker)
    : TrackEventExtensionParser(extension_context),
      trace_context_(context),
      active_processes_tracker_(active_processes_tracker),
      task_file_name_args_key_id_(
          context->storage->InternString("task.posted_from.file_name")),
      task_function_name_args_key_id_(
          context->storage->InternString("task.posted_from.function_name")),
      task_line_number_args_key_id_(
          context->storage->InternString("task.posted_from.line_number")),
      log_message_body_key_id_(
          context->storage->InternString("track_event.log_message.message")),
      log_message_source_location_function_name_key_id_(
          context->storage->InternString(
              "track_event.log_message.function_name")),
      log_message_source_location_file_name_key_id_(
          context->storage->InternString("track_event.log_message.file_name")),
      log_message_source_location_line_number_key_id_(
          context->storage->InternString(
              "track_event.log_message.line_number")),
      log_message_priority_id_(
          context->storage->InternString("track_event.priority")),
      source_location_function_name_key_id_(
          context->storage->InternString("source.function_name")),
      source_location_file_name_key_id_(
          context->storage->InternString("source.file_name")),
      source_location_line_number_key_id_(
          context->storage->InternString("source.line_number")),
      histogram_name_key_id_(
          context->storage->InternString("chrome_histogram_sample.name")) {
  RegisterTrackEventExtension(TrackEvent::kSourceLocationIidFieldNumber);
  RegisterTrackEventExtension(TrackEvent::kTaskExecutionFieldNumber);
  RegisterTrackEventExtension(TrackEvent::kLogMessageFieldNumber);
  RegisterTrackEventExtension(TrackEvent::kScreenshotFieldNumber);
  RegisterTrackEventExtension(TrackEvent::kChromeHistogramSampleFieldNumber);
  RegisterTrackEventExtension(TrackEvent::kChromeActiveProcessesFieldNumber);
  RegisterTrackEventExtension(
      protos::pbzero::GpuTrackEvent::kGpuCorrelationFieldNumber);
}

TrackEventArgFieldParser::~TrackEventArgFieldParser() = default;

TrackEventExtensionParser::Result TrackEventArgFieldParser::OnTrackEventField(
    const TrackEventExtensionField& field,
    const TrackEventFieldContext& event) {
  if (field.id() == protos::pbzero::GpuTrackEvent::kGpuCorrelationFieldNumber) {
    ParseGpuCorrelation(
        field.Cast<protos::pbzero::GpuTrackEvent::kGpuCorrelation>(), event);
    return Result::kIgnored;
  }
  // A row without args takes nothing from these fields.
  if (!event.args) {
    return Result::kIgnored;
  }
  auto log_errors = [this](const base::Status& status) {
    if (status.ok())
      return;
    // Log error but continue parsing the other args.
    trace_context_->stats_tracker->IncrementStats(
        stats::track_event_parser_errors);
    PERFETTO_DLOG("ParseTrackEventArgs error: %s", status.c_message());
  };
  switch (field.id()) {
    case TrackEvent::kSourceLocationIidFieldNumber:
      log_errors(AddSourceLocationArgs(
          field.Cast<TrackEvent::kSourceLocationIid>(), event));
      return Result::kIgnored;
    case TrackEvent::kTaskExecutionFieldNumber:
      log_errors(
          ParseTaskExecution(field.Cast<TrackEvent::kTaskExecution>(), event));
      return Result::kIgnored;
    case TrackEvent::kLogMessageFieldNumber:
      log_errors(ParseLogMessage(field.Cast<TrackEvent::kLogMessage>(), event));
      return Result::kIgnored;
    case TrackEvent::kScreenshotFieldNumber:
      ParseScreenshot(field.Cast<TrackEvent::kScreenshot>(), event);
      return Result::kIgnored;
    case TrackEvent::kChromeHistogramSampleFieldNumber:
      log_errors(ParseHistogramName(
          field.Cast<TrackEvent::kChromeHistogramSample>(), event));
      return Result::kIgnored;
    case TrackEvent::kChromeActiveProcessesFieldNumber:
      ParseActiveProcesses(field.Cast<TrackEvent::kChromeActiveProcesses>(),
                           event);
      return Result::kIgnored;
    default:
      return Result::kIgnored;
  }
}

base::Status TrackEventArgFieldParser::AddSourceLocationArgs(
    uint64_t iid,
    const TrackEventFieldContext& event) {
  if (!iid)
    return base::ErrStatus("SourceLocation with invalid iid");

  auto* decoder = event.sequence_state->LookupInternedMessage<
      protos::pbzero::InternedData::kSourceLocationsFieldNumber,
      protos::pbzero::SourceLocation>(iid);
  if (!decoder)
    return base::ErrStatus("SourceLocation with invalid iid");

  TraceStorage* storage = trace_context_->storage.get();
  std::string file_name = NormalizePathSeparators(decoder->file_name());
  event.args->AddArg(
      source_location_file_name_key_id_,
      Variadic::String(storage->InternString(base::StringView(file_name))));
  event.args->AddArg(
      source_location_function_name_key_id_,
      Variadic::String(storage->InternString(decoder->function_name())));
  event.args->AddArg(source_location_line_number_key_id_,
                     Variadic::UnsignedInteger(decoder->line_number()));
  return base::OkStatus();
}

base::Status TrackEventArgFieldParser::ParseTaskExecution(
    protozero::ConstBytes task_execution,
    const TrackEventFieldContext& event) {
  protos::pbzero::TaskExecution::Decoder task(task_execution);
  uint64_t iid = task.posted_from_iid();
  if (!iid)
    return base::ErrStatus("TaskExecution with invalid posted_from_iid");

  auto* decoder = event.sequence_state->LookupInternedMessage<
      protos::pbzero::InternedData::kSourceLocationsFieldNumber,
      protos::pbzero::SourceLocation>(iid);
  if (!decoder)
    return base::ErrStatus("TaskExecution with invalid posted_from_iid");

  TraceStorage* storage = trace_context_->storage.get();
  std::string file_name = NormalizePathSeparators(decoder->file_name());
  event.args->AddArg(
      task_file_name_args_key_id_,
      Variadic::String(storage->InternString(base::StringView(file_name))));
  event.args->AddArg(
      task_function_name_args_key_id_,
      Variadic::String(storage->InternString(decoder->function_name())));
  event.args->AddArg(task_line_number_args_key_id_,
                     Variadic::UnsignedInteger(decoder->line_number()));
  return base::OkStatus();
}

base::Status TrackEventArgFieldParser::ParseLogMessage(
    protozero::ConstBytes blob,
    const TrackEventFieldContext& event) {
  if (!event.has_utid())
    return base::ErrStatus("LogMessage without thread association");

  TraceStorage* storage = trace_context_->storage.get();
  protos::pbzero::LogMessage::Decoder message(blob);

  std::optional<StringId> body_id = event.sequence_state->InternedStringId(
      protos::pbzero::InternedData::kLogMessageBodyFieldNumber,
      message.body_iid());
  if (!body_id)
    return base::ErrStatus("LogMessage with invalid body_iid");

  const StringId log_message_id = *body_id;
  event.args->AddArg(log_message_body_key_id_,
                     Variadic::String(log_message_id));

  StringId source_location_id = kNullStringId;
  if (message.has_source_location_iid()) {
    auto* source_location_decoder = event.sequence_state->LookupInternedMessage<
        protos::pbzero::InternedData::kSourceLocationsFieldNumber,
        protos::pbzero::SourceLocation>(message.source_location_iid());
    if (!source_location_decoder)
      return base::ErrStatus("LogMessage with invalid source_location_iid");
    const std::string source_location =
        source_location_decoder->file_name().ToStdString() + ":" +
        std::to_string(source_location_decoder->line_number());
    source_location_id =
        storage->InternString(base::StringView(source_location));

    event.args->AddArg(log_message_source_location_file_name_key_id_,
                       Variadic::String(storage->InternString(
                           source_location_decoder->file_name())));
    event.args->AddArg(log_message_source_location_function_name_key_id_,
                       Variadic::String(storage->InternString(
                           source_location_decoder->function_name())));
    event.args->AddArg(
        log_message_source_location_line_number_key_id_,
        Variadic::Integer(source_location_decoder->line_number()));
  }

  // The track event log message doesn't specify any priority. UI never
  // displays priorities < 2 (VERBOSE in android). Let's make all the track
  // event logs show up as INFO.
  int32_t priority = protos::pbzero::AndroidLogPriority::PRIO_INFO;
  if (message.has_prio()) {
    priority = ToAndroidLogPriority(
        static_cast<protos::pbzero::LogMessage::Priority>(message.prio()));
    event.args->AddArg(log_message_priority_id_, Variadic::Integer(priority));
  }

  tables::LogTable::Row log_row;
  log_row.ts = event.ts;
  log_row.utid = event.utid;
  log_row.prio = static_cast<uint32_t>(priority);
  log_row.log_source = storage->InternString("android_logcat");
  log_row.tag = source_location_id != kNullStringId
                    ? std::make_optional(source_location_id)
                    : std::nullopt;
  log_row.msg = log_message_id;
  storage->mutable_log_table()->Insert(log_row);
  return base::OkStatus();
}

base::Status TrackEventArgFieldParser::ParseHistogramName(
    protozero::ConstBytes blob,
    const TrackEventFieldContext& event) {
  protos::pbzero::ChromeHistogramSample::Decoder sample(blob);
  if (!sample.has_name_iid())
    return base::OkStatus();

  if (sample.has_name()) {
    return base::ErrStatus(
        "name is already set for ChromeHistogramSample: only one of name and "
        "name_iid can be set.");
  }

  std::optional<StringId> name_id = event.sequence_state->InternedStringId(
      protos::pbzero::InternedData::kHistogramNamesFieldNumber,
      sample.name_iid());
  if (!name_id)
    return base::ErrStatus("HistogramName with invalid name_iid");

  event.args->AddArg(histogram_name_key_id_, Variadic::String(*name_id));
  return base::OkStatus();
}

void TrackEventArgFieldParser::ParseScreenshot(
    protozero::ConstBytes screenshot_bytes,
    const TrackEventFieldContext& event) {
  TraceStorage* storage = trace_context_->storage.get();
  protos::pbzero::Screenshot::Decoder screenshot(screenshot_bytes);
  if (screenshot.has_jpg_image()) {
    std::string base64_jpg = base::Base64Encode(screenshot.jpg_image().data,
                                                screenshot.jpg_image().size);
    event.args->AddArg(
        storage->InternString("screenshot.jpg_image"),
        Variadic::String(storage->InternString(base::StringView(base64_jpg))));
  }
  if (screenshot.has_pam_image()) {
    std::string base64_pam = base::Base64Encode(screenshot.pam_image().data,
                                                screenshot.pam_image().size);
    event.args->AddArg(
        storage->InternString("screenshot.pam_image"),
        Variadic::String(storage->InternString(base::StringView(base64_pam))));
  }
  if (screenshot.has_ppm_image()) {
    std::string base64_ppm = base::Base64Encode(screenshot.ppm_image().data,
                                                screenshot.ppm_image().size);
    event.args->AddArg(
        storage->InternString("screenshot.ppm_image"),
        Variadic::String(storage->InternString(base::StringView(base64_ppm))));
  }
}

void TrackEventArgFieldParser::ParseGpuCorrelation(
    protozero::ConstBytes blob,
    const TrackEventFieldContext& event) {
  if (event.row_kind != TrackEventFieldContext::RowKind::kSlice) {
    return;
  }
  SliceId slice_id = event.slice_id();
  protos::pbzero::GpuCorrelation::Decoder gpu(blob);
  for (auto it = gpu.render_stage_submission_event_ids(); it; ++it) {
    trace_context_->gpu_tracker->AddRenderStageSubmission(*it, slice_id);
  }
  for (auto it = gpu.render_stage_wait_event_ids(); it; ++it) {
    trace_context_->gpu_tracker->AddRenderStageWait(*it, slice_id);
  }
}

void TrackEventArgFieldParser::ParseActiveProcesses(
    protozero::ConstBytes blob,
    const TrackEventFieldContext& event) {
  protos::pbzero::ChromeActiveProcesses::Decoder message(blob);
  for (auto it = message.pid(); it; ++it) {
    UniquePid upid = trace_context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(*it));
    active_processes_tracker_->AddActiveProcessMetadata(event.ts, upid);
  }
}

}  // namespace perfetto::trace_processor
