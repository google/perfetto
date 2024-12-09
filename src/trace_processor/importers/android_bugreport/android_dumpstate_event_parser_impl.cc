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

#include "src/trace_processor/importers/android_bugreport/android_dumpstate_event_parser_impl.h"

#include <cstdint>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/android_bugreport/android_battery_stats_history_string_tracker.h"
#include "src/trace_processor/importers/android_bugreport/android_dumpstate_event.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {

namespace {
base::StatusOr<std::string> GetEventFromShortName(base::StringView short_name) {
  static const base::NoDestructor<
      std::unordered_map<base::StringView, std::string> >
      checkin_event_name_to_enum(
          std::unordered_map<base::StringView, std::string>({
              {"Enl", "null"},       {"Epr", "proc"},
              {"Efg", "fg"},         {"Etp", "top"},
              {"Esy", "sync"},       {"Ewl", "wake_lock_in"},
              {"Ejb", "job"},        {"Eur", "user"},
              {"Euf", "userfg"},     {"Ecn", "conn"},
              {"Eac", "active"},     {"Epi", "pkginst"},
              {"Epu", "pkgunin"},    {"Eal", "alarm"},
              {"Est", "stats"},      {"Eai", "pkginactive"},
              {"Eaa", "pkgactive"},  {"Etw", "tmpwhitelist"},
              {"Esw", "screenwake"}, {"Ewa", "wakeupap"},
              {"Elw", "longwake"},   {"Eec", "est_capacity"},
          }));
  auto result = checkin_event_name_to_enum.ref().find(short_name);
  if (result == checkin_event_name_to_enum.ref().end()) {
    return base::ErrStatus("Failed to find historty event name mapping");
  }
  return result->second;
}

struct StateStringTranslationInfo {
  const std::string long_name;
  const std::unordered_map<base::StringView, uint64_t> short_string_to_value;
};

base::StatusOr<std::string> GetStateAndValueFromShortName(
    base::StringView state_short_name,
    base::StringView value_short_name,
    uint64_t* value_out) {
  // Mappings of all the state checkin names from BatteryStats.java and their
  // corresponding value mappings
  static const base::NoDestructor<
      std::unordered_map<base::StringView, StateStringTranslationInfo> >
      checkin_state_name_to_enum_and_values(
          std::unordered_map<base::StringView, StateStringTranslationInfo>(
              {{"r", {"running", {}}},
               {"w", {"wake_lock", {}}},
               {"s", {"sensor", {}}},
               {"g", {"gps", {}}},
               {"Wl", {"wifi_full_lock", {}}},
               {"Ws", {"wifi_scan", {}}},
               {"Wm", {"wifi_multicast", {}}},
               {"Wr", {"wifi_radio", {}}},
               {"Pr", {"mobile_radio", {}}},
               {"Psc", {"phone_scanning", {}}},
               {"a", {"audio", {}}},
               {"S", {"screen", {}}},
               {"BP", {"plugged", {}}},
               {"Sd", {"screen_doze", {}}},
               {"Pcn",
                {"data_conn",
                 {
                     {"oos", 0},     {"gprs", 1},    {"edge", 2},
                     {"umts", 3},    {"cdma", 4},    {"evdo_0", 5},
                     {"evdo_A", 6},  {"1xrtt", 7},   {"hsdpa", 8},
                     {"hsupa", 9},   {"hspa", 10},   {"iden", 11},
                     {"evdo_b", 12}, {"lte", 13},    {"ehrpd", 14},
                     {"hspap", 15},  {"gsm", 16},    {"td_scdma", 17},
                     {"iwlan", 18},  {"lte_ca", 19}, {"nr", 20},
                     {"emngcy", 21}, {"other", 22},
                 }}},
               {"Pst",
                {"phone_state",
                 {
                     {"in", 0},
                     {"out", 1},
                     {"em", 2},
                     {"off", 3},
                 }}},
               {"Pss", {"phone_signal_strength", {}}},
               {"Sb", {"brightness", {}}},
               {"ps", {"power_save", {}}},
               {"v", {"video", {}}},
               {"Ww", {"wifi_running", {}}},
               {"W", {"wifi", {}}},
               {"fl", {"flashlight", {}}},
               {"di",
                {"device_idle",
                 {
                     {"off", 0},
                     {"light", 1},
                     {"full", 2},
                     {"???", 3},
                 }}},
               {"ch", {"charging", {}}},
               {"Ud", {"usb_data", {}}},
               {"Pcl", {"phone_in_call", {}}},
               {"b", {"bluetooth", {}}},
               {"Wss", {"wifi_signal_strength", {}}},
               {"Wsp",
                {"wifi_suppl",
                 {
                     {"inv", 0},
                     {"dsc", 1},
                     {"dis", 2},
                     {"inact", 3},
                     {"scan", 4},
                     {"auth", 5},
                     {"ascing", 6},
                     {"asced", 7},
                     {"4-way", 8},
                     {"group", 9},
                     {"compl", 10},
                     {"dorm", 11},
                     {"uninit", 12},
                 }}},
               {"ca", {"camera", {}}},
               {"bles", {"ble_scan", {}}},
               {"Chtp", {"cellular_high_tx_power", {}}},
               {"Gss",
                {"gps_signal_quality",
                 {
                     {"poor", 0},
                     {"good", 1},
                     {"none", 2},
                 }}},
               {"nrs", {"nr_state", {}}}}));

  auto result =
      checkin_state_name_to_enum_and_values.ref().find(state_short_name);
  if (result == checkin_state_name_to_enum_and_values.ref().end()) {
    return base::ErrStatus("Failed to find state short to long name mapping");
  }

  StateStringTranslationInfo translation_info = result->second;

  // If caller isn't requesting a value, just return the item type.
  if (value_out == nullptr) {
    return translation_info.long_name;
  }

  // If the value short name is already a number, just do a direct conversion
  std::optional<uint64_t> possible_int_value =
      base::StringToUInt64(value_short_name.ToStdString());
  if (possible_int_value.has_value()) {
    *value_out = possible_int_value.value();
    return translation_info.long_name;
  }
  // value has a non-numerical string, so translate it
  auto short_name_mapping =
      translation_info.short_string_to_value.find(value_short_name);
  if (short_name_mapping == translation_info.short_string_to_value.end()) {
    return base::ErrStatus("Failed to translate value for state");
  }
  *value_out = short_name_mapping->second;
  return translation_info.long_name;
}

base::StatusOr<uint64_t> StringToStatusOrUInt64(base::StringView str) {
  std::optional<uint64_t> possible_result =
      base::StringToUInt64(str.ToStdString());
  if (!possible_result.has_value()) {
    return base::ErrStatus("Failed to convert string to uint64_t");
  }
  return possible_result.value();
}

}  // namespace

AndroidDumpstateEventParserImpl::~AndroidDumpstateEventParserImpl() = default;

void AndroidDumpstateEventParserImpl::ParseAndroidDumpstateEvent(
    int64_t ts,
    AndroidDumpstateEvent event) {
  switch (event.type) {
    case AndroidDumpstateEvent::EventType::kBatteryStatsHistoryEvent:
      ProcessBatteryStatsHistoryItem(ts, event.raw_event);
      return;
    case AndroidDumpstateEvent::EventType::kNull:
      return;
  }
}

base::Status AndroidDumpstateEventParserImpl::ProcessBatteryStatsHistoryItem(
    int64_t ts,
    const std::string& raw_event) {
  // TODO: migrate to future StringViewSplitter when availabile.
  base::StringSplitter splitter(raw_event, '=');
  base::StringView key =
      base::StringView(splitter.Next() ? splitter.cur_token() : "");
  base::StringView value =
      base::StringView(splitter.Next() ? splitter.cur_token() : "");

  AndroidBatteryStatsHistoryStringTracker* history_string_tracker =
      AndroidBatteryStatsHistoryStringTracker::GetOrCreate(context_);

  std::string item_name;
  if (key.StartsWith("+E") || key.StartsWith("-E") || key.StartsWith("E")) {
    // Process a history event
    base::StringView prefix = "";
    if (key.at(0) == '+' || key.at(0) == '-') {
      prefix = key.substr(0, 1);
      key = key.substr(1);
    }
    ASSIGN_OR_RETURN(item_name, GetEventFromShortName(key));
    ASSIGN_OR_RETURN(uint64_t hsp_index, StringToStatusOrUInt64(value));
    const int32_t uid = history_string_tracker->GetUid(hsp_index);
    const std::string& event_str = history_string_tracker->GetString(hsp_index);
    StringId track_name_id = context_->storage->InternString(
        std::string("battery_stats.").append(item_name));
    const std::string slice_name = prefix.ToStdString()
                                       .append(item_name)
                                       .append("=")
                                       .append(std::to_string(uid))
                                       .append(":\"")
                                       .append(event_str)
                                       .append("\"");
    StringId name_id = context_->storage->InternString(slice_name);
    AsyncTrackSetTracker::TrackSetId track_set_id =
        context_->async_track_set_tracker->InternGlobalTrackSet(track_name_id);
    TrackId track_id =
        context_->async_track_set_tracker->Scoped(track_set_id, ts, 0);
    context_->slice_tracker->Scoped(ts, track_id, kNullStringId, name_id, 0);
    return base::OkStatus();
  } else if ((key.StartsWith("+") || key.StartsWith("-")) && value.empty()) {
    // Process a history state of the form "+state" or "-state"

    // To match behavior of the battery stats atrace implementation, avoid
    // including Wakelock events in the trace
    if (key == "+w" || key == "-w") {
      return base::OkStatus();
    }

    ASSIGN_OR_RETURN(item_name,
                     GetStateAndValueFromShortName(key.substr(1), "", nullptr));
    TrackId track = context_->track_tracker->InternTrack(
        tracks::kAndroidBatteryStatsBlueprint,
        tracks::Dimensions(
            base::StringView(std::string("battery_stats.").append(item_name))));
    context_->event_tracker->PushCounter(ts, (key.at(0) == '+') ? 1.0 : 0.0,
                                         track);
    // Also add a screen events to the screen state track
    if (item_name == "screen") {
      track = context_->track_tracker->InternTrack(
          tracks::kAndroidScreenStateBlueprint);
      // battery_stats.screen event is 0 for off and 1 for on, but the
      // ScreenState track uses the convention 1 for off and 2 for on, so add
      // 1 to the current counter value.
      context_->event_tracker->PushCounter(
          ts, static_cast<double>((key.at(0) == '+') ? 2.0 : 1.0), track);
    }
    return base::OkStatus();
  } else if (!key.StartsWith("+") && !key.StartsWith("-") && !value.empty()) {
    // AndroidProbesParser will use the empty string for the battery name if no
    // battery name is associated with the data, which is common on most pixel
    // phones. Adopt the same convention here. Battery stats does not provide
    // a battery name in the checking format, so we'll always have an unknown
    // battery.
    const base::StringView kUnknownBatteryName = "";

    // process history state of form "state=12345" or "state=abcde"
    TrackId counter_track;
    uint64_t counter_value;
    base::StatusOr<std::string> possible_history_state_item =
        GetStateAndValueFromShortName(key, value, &counter_value);
    if (possible_history_state_item.ok()) {
      item_name = possible_history_state_item.value();
      counter_track = context_->track_tracker->InternTrack(
          tracks::kAndroidBatteryStatsBlueprint,
          tracks::Dimensions(base::StringView(
              std::string("battery_stats.").append(item_name))));
    } else if (key == "Bl") {
      counter_track = context_->track_tracker->InternTrack(
          tracks::kBatteryCounterBlueprint,
          tracks::Dimensions(kUnknownBatteryName, "capacity_pct"));
      ASSIGN_OR_RETURN(counter_value, StringToStatusOrUInt64(value));
    } else if (key == "Bcc") {
      counter_track = context_->track_tracker->InternTrack(
          tracks::kBatteryCounterBlueprint,
          tracks::Dimensions(kUnknownBatteryName, "charge_uah"));
      ASSIGN_OR_RETURN(counter_value, StringToStatusOrUInt64(value));
      // battery stats gives us charge in milli-amp-hours, but the track
      // expects the value to be in micro-amp-hours
      counter_value *= 1000;
    } else if (key == "Bv") {
      counter_track = context_->track_tracker->InternTrack(
          tracks::kBatteryCounterBlueprint,
          tracks::Dimensions(kUnknownBatteryName, "voltage_uv"));
      ASSIGN_OR_RETURN(counter_value, StringToStatusOrUInt64(value));
      // battery stats gives us charge in milli-volts, but the track
      // expects the value to be in micro-volts
      counter_value *= 1000;
    } else if (key == "Bs") {
      static constexpr auto kBatteryStatusBlueprint = tracks::CounterBlueprint(
          "battery_status", tracks::UnknownUnitBlueprint(),
          tracks::DimensionBlueprints(),
          tracks::StaticNameBlueprint("BatteryStatus"));
      counter_track =
          context_->track_tracker->InternTrack(kBatteryStatusBlueprint);
      switch (value.at(0)) {
        case '?':
          counter_value = 1;  // BatteryManager.BATTERY_STATUS_UNKNOWN
          break;
        case 'c':
          counter_value = 2;  // BatteryManager.BATTERY_STATUS_CHARGING
          break;
        case 'd':
          counter_value = 3;  // BatteryManager.BATTERY_STATUS_DISCHARGING
          break;
        case 'n':
          counter_value = 4;  // BatteryManager.BATTERY_STATUS_NOT_CHARGING
          break;
        case 'f':
          counter_value = 5;  // BatteryManager.BATTERY_STATUS_FULL
          break;
        default:
          PERFETTO_ELOG("unknown battery status: %c", value.at(0));
          counter_value = 0;  // not a valid enum
      }
    } else if (key == "Bp") {
      static constexpr auto kPluggedStatusBluePrint = tracks::CounterBlueprint(
          "battery_plugged_status", tracks::UnknownUnitBlueprint(),
          tracks::DimensionBlueprints(),
          tracks::StaticNameBlueprint("PlugType"));
      counter_track =
          context_->track_tracker->InternTrack(kPluggedStatusBluePrint);
      switch (value.at(0)) {
        case 'n':
          counter_value = 0;  // BatteryManager.BATTERY_PLUGGED_NONE
          break;
        case 'a':
          counter_value = 1;  // BatteryManager.BATTERY_PLUGGED_AC
          break;
        case 'u':
          counter_value = 2;  // BatteryManager.BATTERY_PLUGGED_USB
          break;
        case 'w':
          counter_value = 4;  // BatteryManager.BATTERY_PLUGGED_WIRELESS
          break;
        default:
          counter_value = 0;  // BatteryManager.BATTERY_PLUGGED_NONE
      }
    } else {
      return base::ErrStatus("Unhandled event");
    }

    context_->event_tracker->PushCounter(ts, static_cast<double>(counter_value),
                                         counter_track);
    return base::OkStatus();
  } else {
    return base::ErrStatus("Unhandled event");
  }
}

}  // namespace perfetto::trace_processor
