/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/android_probes_parser.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/proto/android_probes_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/common/android_log_constants.pbzero.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/android/android_game_intervention_list.pbzero.h"
#include "protos/perfetto/trace/android/android_log.pbzero.h"
#include "protos/perfetto/trace/android/android_system_property.pbzero.h"
#include "protos/perfetto/trace/android/initial_display_state.pbzero.h"
#include "protos/perfetto/trace/power/android_energy_estimation_breakdown.pbzero.h"
#include "protos/perfetto/trace/power/android_entity_state_residency.pbzero.h"
#include "protos/perfetto/trace/power/battery_counters.pbzero.h"
#include "protos/perfetto/trace/power/power_rails.pbzero.h"

namespace perfetto::trace_processor {

AndroidProbesParser::AndroidProbesParser(TraceProcessorContext* context)
    : context_(context),
      batt_charge_id_(context->storage->InternString("batt.charge_uah")),
      batt_capacity_id_(context->storage->InternString("batt.capacity_pct")),
      batt_current_id_(context->storage->InternString("batt.current_ua")),
      batt_current_avg_id_(
          context->storage->InternString("batt.current.avg_ua")),
      batt_voltage_id_(context->storage->InternString("batt.voltage_uv")),
      batt_power_id_(context->storage->InternString("batt.power_mw")),
      screen_state_id_(context->storage->InternString("ScreenState")),
      device_state_id_(context->storage->InternString("DeviceStateChanged")),
      battery_status_id_(context->storage->InternString("BatteryStatus")),
      plug_type_id_(context->storage->InternString("PlugType")),
      rail_packet_timestamp_id_(context->storage->InternString("packet_ts")),
      energy_consumer_id_(
          context_->storage->InternString("energy_consumer_id")),
      consumer_type_id_(context_->storage->InternString("consumer_type")),
      ordinal_id_(context_->storage->InternString("ordinal")) {}

void AndroidProbesParser::ParseBatteryCounters(int64_t ts, ConstBytes blob) {
  protos::pbzero::BatteryCounters::Decoder evt(blob.data, blob.size);
  StringId batt_charge_id = batt_charge_id_;
  StringId batt_capacity_id = batt_capacity_id_;
  StringId batt_current_id = batt_current_id_;
  StringId batt_current_avg_id = batt_current_avg_id_;
  StringId batt_voltage_id = batt_voltage_id_;
  StringId batt_power_id = batt_power_id_;
  if (evt.has_name()) {
    std::string batt_name = evt.name().ToStdString();
    batt_charge_id = context_->storage->InternString(base::StringView(
        std::string("batt.").append(batt_name).append(".charge_uah")));
    batt_capacity_id = context_->storage->InternString(base::StringView(
        std::string("batt.").append(batt_name).append(".capacity_pct")));
    batt_current_id = context_->storage->InternString(base::StringView(
        std::string("batt.").append(batt_name).append(".current_ua")));
    batt_current_avg_id = context_->storage->InternString(base::StringView(
        std::string("batt.").append(batt_name).append(".current.avg_ua")));
    batt_voltage_id = context_->storage->InternString(base::StringView(
        std::string("batt.").append(batt_name).append(".voltage_uv")));
    batt_power_id = context_->storage->InternString(base::StringView(
        std::string("batt.").append(batt_name).append(".power_mw")));
  }
  if (evt.has_charge_counter_uah()) {
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_charge_id);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(evt.charge_counter_uah()), track);
  } else if (evt.has_energy_counter_uwh() && evt.has_voltage_uv()) {
    // Calculate charge counter from energy counter and voltage.
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_charge_id);
    auto energy = evt.energy_counter_uwh();
    auto voltage = evt.voltage_uv();
    if (voltage > 0) {
      context_->event_tracker->PushCounter(
          ts, static_cast<double>(energy * 1000000 / voltage), track);
    }
  }

  if (evt.has_capacity_percent()) {
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_capacity_id);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(evt.capacity_percent()), track);
  }
  if (evt.has_current_ua()) {
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_current_id);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(evt.current_ua()), track);
  }
  if (evt.has_current_avg_ua()) {
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_current_avg_id);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(evt.current_avg_ua()), track);
  }
  if (evt.has_voltage_uv()) {
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_voltage_id);
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(evt.voltage_uv()), track);
  }
  if (evt.has_current_ua() && evt.has_voltage_uv()) {
    // Calculate power from current and voltage.
    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, batt_power_id);
    auto current = evt.current_ua();
    auto voltage = evt.voltage_uv();
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(current * voltage / 1000000000), track);
  }
}

void AndroidProbesParser::ParsePowerRails(int64_t ts,
                                          uint64_t trace_packet_ts,
                                          ConstBytes blob) {
  protos::pbzero::PowerRails::Decoder evt(blob.data, blob.size);

  // Descriptors should have been processed at tokenization time.
  PERFETTO_DCHECK(evt.has_energy_data());

  // Because we have some special code in the tokenization phase, we
  // will only every get one EnergyData message per packet. Therefore,
  // we can just read the data directly.
  auto it = evt.energy_data();
  protos::pbzero::PowerRails::EnergyData::Decoder desc(*it);

  auto* tracker = AndroidProbesTracker::GetOrCreate(context_);
  auto opt_track = tracker->GetPowerRailTrack(desc.index());
  if (opt_track.has_value()) {
    // The tokenization makes sure that this field is always present and
    // is equal to the packet's timestamp that was passed to us via the sorter.
    PERFETTO_DCHECK(desc.has_timestamp_ms());
    PERFETTO_DCHECK(ts / 1000000 == static_cast<int64_t>(desc.timestamp_ms()));
    auto maybe_counter_id = context_->event_tracker->PushCounter(
        ts, static_cast<double>(desc.energy()), *opt_track);
    if (maybe_counter_id) {
      context_->args_tracker->AddArgsTo(*maybe_counter_id)
          .AddArg(rail_packet_timestamp_id_,
                  Variadic::UnsignedInteger(trace_packet_ts));
    }
  } else {
    context_->storage->IncrementStats(stats::power_rail_unknown_index);
  }

  // DCHECK that we only got one message.
  PERFETTO_DCHECK(!++it);
}

void AndroidProbesParser::ParseEnergyBreakdown(int64_t ts, ConstBytes blob) {
  protos::pbzero::AndroidEnergyEstimationBreakdown::Decoder event(blob.data,
                                                                  blob.size);

  if (!event.has_energy_consumer_id() || !event.has_energy_uws()) {
    context_->storage->IncrementStats(stats::energy_breakdown_missing_values);
    return;
  }

  auto consumer_id = event.energy_consumer_id();
  auto* tracker = AndroidProbesTracker::GetOrCreate(context_);
  auto energy_consumer_specs =
      tracker->GetEnergyBreakdownDescriptor(consumer_id);

  if (!energy_consumer_specs) {
    context_->storage->IncrementStats(stats::energy_breakdown_missing_values);
    return;
  }

  auto total_energy = static_cast<double>(event.energy_uws());
  auto consumer_name = energy_consumer_specs->name;
  auto consumer_type = energy_consumer_specs->type;
  auto ordinal = energy_consumer_specs->ordinal;

  TrackId energy_track = context_->track_tracker->InternSingleDimensionTrack(
      tracks::android_energy_estimation_breakdown, energy_consumer_id_,
      Variadic::Integer(consumer_id),
      TrackTracker::LegacyStringIdName{consumer_name},
      [&](ArgsTracker::BoundInserter& inserter) {
        inserter.AddArg(consumer_type_id_, Variadic::String(consumer_type));
        inserter.AddArg(ordinal_id_, Variadic::Integer(ordinal));
      });
  context_->event_tracker->PushCounter(ts, total_energy, energy_track);

  // Consumers providing per-uid energy breakdown
  for (auto it = event.per_uid_breakdown(); it; ++it) {
    protos::pbzero::AndroidEnergyEstimationBreakdown_EnergyUidBreakdown::Decoder
        breakdown(*it);

    if (!breakdown.has_uid() || !breakdown.has_energy_uws()) {
      context_->storage->IncrementStats(
          stats::energy_uid_breakdown_missing_values);
      continue;
    }

    auto builder = context_->track_tracker->CreateDimensionsBuilder();
    builder.AppendUid(breakdown.uid());
    builder.AppendDimension(energy_consumer_id_,
                            Variadic::Integer(consumer_id));
    TrackId energy_uid_track = context_->track_tracker->InternTrack(
        tracks::android_energy_estimation_breakdown_per_uid,
        std::move(builder).Build(),
        TrackTracker::LegacyStringIdName{consumer_name});
    context_->event_tracker->PushCounter(
        ts, static_cast<double>(breakdown.energy_uws()), energy_uid_track);
  }
}

void AndroidProbesParser::ParseEntityStateResidency(int64_t ts,
                                                    ConstBytes blob) {
  protos::pbzero::EntityStateResidency::Decoder event(blob.data, blob.size);

  if (!event.has_residency()) {
    context_->storage->IncrementStats(stats::entity_state_residency_invalid);
    return;
  }

  auto* tracker = AndroidProbesTracker::GetOrCreate(context_);

  for (auto it = event.residency(); it; ++it) {
    protos::pbzero::EntityStateResidency::StateResidency::Decoder residency(
        *it);

    auto entity_state = tracker->GetEntityStateDescriptor(
        residency.entity_index(), residency.state_index());
    if (!entity_state) {
      context_->storage->IncrementStats(
          stats::entity_state_residency_lookup_failed);
      return;
    }

    TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
        TrackTracker::Group::kPower, entity_state->overall_name);
    context_->event_tracker->PushCounter(
        ts, double(residency.total_time_in_state_ms()), track);
  }
}

void AndroidProbesParser::ParseAndroidLogPacket(ConstBytes blob) {
  protos::pbzero::AndroidLogPacket::Decoder packet(blob.data, blob.size);
  for (auto it = packet.events(); it; ++it)
    ParseAndroidLogEvent(*it);

  if (packet.has_stats())
    ParseAndroidLogStats(packet.stats());
}

void AndroidProbesParser::ParseAndroidLogEvent(ConstBytes blob) {
  // TODO(primiano): Add events and non-stringified fields to the "raw" table.
  protos::pbzero::AndroidLogPacket::LogEvent::Decoder evt(blob.data, blob.size);
  int64_t ts = static_cast<int64_t>(evt.timestamp());
  uint32_t pid = static_cast<uint32_t>(evt.pid());
  uint32_t tid = static_cast<uint32_t>(evt.tid());
  uint8_t prio = static_cast<uint8_t>(evt.prio());
  StringId tag_id = context_->storage->InternString(
      evt.has_tag() ? evt.tag() : base::StringView());
  StringId msg_id = context_->storage->InternString(
      evt.has_message() ? evt.message() : base::StringView());

  char arg_msg[4096];
  char* arg_str = &arg_msg[0];
  *arg_str = '\0';
  auto arg_avail = [&arg_msg, &arg_str]() {
    size_t used = static_cast<size_t>(arg_str - arg_msg);
    PERFETTO_CHECK(used <= sizeof(arg_msg));
    return sizeof(arg_msg) - used;
  };
  for (auto it = evt.args(); it; ++it) {
    protos::pbzero::AndroidLogPacket::LogEvent::Arg::Decoder arg(*it);
    if (!arg.has_name())
      continue;
    arg_str += base::SprintfTrunc(arg_str, arg_avail(),
                                  " %.*s=", static_cast<int>(arg.name().size),
                                  arg.name().data);
    if (arg.has_string_value()) {
      arg_str += base::SprintfTrunc(arg_str, arg_avail(), "\"%.*s\"",
                                    static_cast<int>(arg.string_value().size),
                                    arg.string_value().data);
    } else if (arg.has_int_value()) {
      arg_str +=
          base::SprintfTrunc(arg_str, arg_avail(), "%" PRId64, arg.int_value());
    } else if (arg.has_float_value()) {
      arg_str += base::SprintfTrunc(arg_str, arg_avail(), "%f",
                                    static_cast<double>(arg.float_value()));
    }
  }

  if (prio == 0)
    prio = protos::pbzero::AndroidLogPriority::PRIO_INFO;

  if (arg_str != &arg_msg[0]) {
    PERFETTO_DCHECK(msg_id.is_null());
    // Skip the first space char (" foo=1 bar=2" -> "foo=1 bar=2").
    msg_id = context_->storage->InternString(&arg_msg[1]);
  }
  UniquePid utid = tid ? context_->process_tracker->UpdateThread(tid, pid) : 0;
  base::StatusOr<int64_t> trace_time = context_->clock_tracker->ToTraceTime(
      protos::pbzero::BUILTIN_CLOCK_REALTIME, ts);
  if (!trace_time.ok()) {
    static std::atomic<uint32_t> dlog_count(0);
    if (dlog_count++ < 10)
      PERFETTO_DLOG("%s", trace_time.status().c_message());
    return;
  }

  // Log events are NOT required to be sorted by trace_time. The virtual table
  // will take care of sorting on-demand.
  context_->storage->mutable_android_log_table()->Insert(
      {trace_time.value(), utid, prio, tag_id, msg_id});
}

void AndroidProbesParser::ParseAndroidLogStats(ConstBytes blob) {
  protos::pbzero::AndroidLogPacket::Stats::Decoder evt(blob.data, blob.size);
  if (evt.has_num_failed()) {
    context_->storage->SetStats(stats::android_log_num_failed,
                                static_cast<int64_t>(evt.num_failed()));
  }

  if (evt.has_num_skipped()) {
    context_->storage->SetStats(stats::android_log_num_skipped,
                                static_cast<int64_t>(evt.num_skipped()));
  }

  if (evt.has_num_total()) {
    context_->storage->SetStats(stats::android_log_num_total,
                                static_cast<int64_t>(evt.num_total()));
  }
}

void AndroidProbesParser::ParseStatsdMetadata(ConstBytes blob) {
  protos::pbzero::TraceConfig::StatsdMetadata::Decoder metadata(blob.data,
                                                                blob.size);
  if (metadata.has_triggering_subscription_id()) {
    context_->metadata_tracker->SetMetadata(
        metadata::statsd_triggering_subscription_id,
        Variadic::Integer(metadata.triggering_subscription_id()));
  }
}

void AndroidProbesParser::ParseAndroidGameIntervention(ConstBytes blob) {
  protos::pbzero::AndroidGameInterventionList::Decoder intervention_list(
      blob.data, blob.size);
  constexpr static int kGameModeStandard = 1;
  constexpr static int kGameModePerformance = 2;
  constexpr static int kGameModeBattery = 3;

  context_->storage->SetStats(stats::game_intervention_has_read_errors,
                              intervention_list.read_error());
  context_->storage->SetStats(stats::game_intervention_has_parse_errors,
                              intervention_list.parse_error());

  for (auto pkg_it = intervention_list.game_packages(); pkg_it; ++pkg_it) {
    protos::pbzero::AndroidGameInterventionList_GamePackageInfo::Decoder
        game_pkg(*pkg_it);
    int64_t uid = static_cast<int64_t>(game_pkg.uid());
    int32_t cur_mode = static_cast<int32_t>(game_pkg.current_mode());

    bool is_standard_mode = false;
    std::optional<double> standard_downscale;
    std::optional<int32_t> standard_angle;
    std::optional<double> standard_fps;

    bool is_performance_mode = false;
    std::optional<double> perf_downscale;
    std::optional<int32_t> perf_angle;
    std::optional<double> perf_fps;

    bool is_battery_mode = false;
    std::optional<double> battery_downscale;
    std::optional<int32_t> battery_angle;
    std::optional<double> battery_fps;

    for (auto mode_it = game_pkg.game_mode_info(); mode_it; ++mode_it) {
      protos::pbzero::AndroidGameInterventionList_GameModeInfo::Decoder
          game_mode(*mode_it);

      uint32_t mode_num = game_mode.mode();
      if (mode_num == kGameModeStandard) {
        is_standard_mode = true;
        standard_downscale =
            static_cast<double>(game_mode.resolution_downscale());
        standard_angle = game_mode.use_angle();
        standard_fps = static_cast<double>(game_mode.fps());
      } else if (mode_num == kGameModePerformance) {
        is_performance_mode = true;
        perf_downscale = static_cast<double>(game_mode.resolution_downscale());
        perf_angle = game_mode.use_angle();
        perf_fps = static_cast<double>(game_mode.fps());
      } else if (mode_num == kGameModeBattery) {
        is_battery_mode = true;
        battery_downscale =
            static_cast<double>(game_mode.resolution_downscale());
        battery_angle = game_mode.use_angle();
        battery_fps = static_cast<double>(game_mode.fps());
      }
    }

    context_->storage->mutable_android_game_intervenion_list_table()->Insert(
        {context_->storage->InternString(game_pkg.name()), uid, cur_mode,
         is_standard_mode, standard_downscale, standard_angle, standard_fps,
         is_performance_mode, perf_downscale, perf_angle, perf_fps,
         is_battery_mode, battery_downscale, battery_angle, battery_fps});
  }
}

void AndroidProbesParser::ParseInitialDisplayState(int64_t ts,
                                                   ConstBytes blob) {
  protos::pbzero::InitialDisplayState::Decoder state(blob.data, blob.size);

  TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
      TrackTracker::Group::kDeviceState, screen_state_id_);
  context_->event_tracker->PushCounter(ts, state.display_state(), track);
}

void AndroidProbesParser::ParseAndroidSystemProperty(int64_t ts,
                                                     ConstBytes blob) {
  protos::pbzero::AndroidSystemProperty::Decoder properties(blob.data,
                                                            blob.size);
  for (auto it = properties.values(); it; ++it) {
    protos::pbzero::AndroidSystemProperty::PropertyValue::Decoder kv(*it);
    base::StringView name(kv.name());
    std::optional<StringId> mapped_name_id;

    if (name == "debug.tracing.device_state") {
      auto state = kv.value();

      StringId state_id = context_->storage->InternString(state);
      auto track_set_id =
          context_->async_track_set_tracker->InternGlobalTrackSet(
              device_state_id_);
      TrackId track_id =
          context_->async_track_set_tracker->Scoped(track_set_id, ts, 0);
      context_->slice_tracker->Scoped(ts, track_id, kNullStringId, state_id, 0);
    } else if (name.StartsWith("debug.tracing.battery_stats.") ||
               name == "debug.tracing.mcc" || name == "debug.tracing.mnc" ||
               name == "debug.tracing.desktop_mode_visible_tasks") {
      StringId name_id = context_->storage->InternString(
          name.substr(strlen("debug.tracing.")));
      std::optional<int32_t> state =
          base::StringToInt32(kv.value().ToStdString());
      if (state) {
        TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
            TrackTracker::Group::kNetwork, name_id);
        context_->event_tracker->PushCounter(ts, *state, track);
      }
    } else if (name == "debug.tracing.screen_state") {
      mapped_name_id = screen_state_id_;
    } else if (name == "debug.tracing.battery_status") {
      mapped_name_id = battery_status_id_;
    } else if (name == "debug.tracing.plug_type") {
      mapped_name_id = plug_type_id_;
    }
    if (mapped_name_id) {
      std::optional<int32_t> state =
          base::StringToInt32(kv.value().ToStdString());
      if (state) {
        TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
            TrackTracker::Group::kDeviceState, *mapped_name_id);
        context_->event_tracker->PushCounter(ts, *state, track);
      }
    }
  }
}

}  // namespace perfetto::trace_processor
