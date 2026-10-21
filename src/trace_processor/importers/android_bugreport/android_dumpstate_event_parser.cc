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

#include "src/trace_processor/importers/android_bugreport/android_dumpstate_event_parser.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/string_view_splitter.h"
#include "src/trace_processor/importers/android_bugreport/android_battery_stats_history_string_tracker.h"
#include "src/trace_processor/importers/android_bugreport/android_dumpstate_event.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

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
              {"Esc", "state"},      {"Eds", "display_state_changed"},
          }));
  auto result = checkin_event_name_to_enum.ref().find(short_name);
  if (result == checkin_event_name_to_enum.ref().end()) {
    return base::ErrStatus("Failed to find historty event name mapping");
  }
  return result->second;
}

struct StateStringTranslationInfo {
  const std::string long_name;
  const std::unordered_map<base::StringView, int64_t> short_string_to_value;
};

base::StatusOr<std::string> GetStateAndValueFromShortName(
    base::StringView state_short_name,
    base::StringView value_short_name,
    int64_t* value_out) {
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
  std::optional<int64_t> possible_int_value =
      base::StringViewToInt64(value_short_name);
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

base::StatusOr<int64_t> StringToStatusOrInt64(base::StringView str) {
  std::optional<int64_t> possible_result = base::StringViewToInt64(str);
  if (!possible_result.has_value()) {
    return base::ErrStatus("Failed to convert string to int64_t");
  }
  return possible_result.value();
}

// Parses a uid either given as a number or formatted by
// UserHandle.formatUid() (e.g. "u0a123" for uid 10123, "u10s1000" for uid
// 1001000).
std::optional<int32_t> ParseUid(base::StringView str) {
  std::optional<int32_t> uid = base::StringViewToInt32(str);
  if (uid.has_value()) {
    return uid;
  }
  if (str.size() < 4 || str.at(0) != 'u') {
    return std::nullopt;
  }
  size_t type_pos = 1;
  while (type_pos < str.size() && str.at(type_pos) >= '0' &&
         str.at(type_pos) <= '9') {
    type_pos++;
  }
  if (type_pos == 1 || type_pos + 1 >= str.size()) {
    return std::nullopt;
  }
  std::optional<int32_t> user =
      base::StringViewToInt32(str.substr(1, type_pos - 1));
  std::optional<int32_t> id = base::StringViewToInt32(str.substr(type_pos + 1));
  if (!user.has_value() || !id.has_value()) {
    return std::nullopt;
  }
  constexpr int32_t kPerUserRange = 100000;     // UserHandle.PER_USER_RANGE
  constexpr int32_t kFirstAppUid = 10000;       // Process.FIRST_APPLICATION_UID
  constexpr int32_t kFirstIsolatedUid = 99000;  // Process.FIRST_ISOLATED_UID
  int32_t app_id;
  switch (str.at(type_pos)) {
    case 'a':
      app_id = kFirstAppUid + *id;
      break;
    case 'i':
      app_id = kFirstIsolatedUid + *id;
      break;
    case 's':
      app_id = *id;
      break;
    default:
      return std::nullopt;
  }
  return *user * kPerUserRange + app_id;
}

// Battery temperature is reported in tenths of a degree Celsius.
constexpr auto kBatteryTemperatureBlueprint =
    tracks::CounterBlueprint("battery_temperature",
                             tracks::StaticUnitBlueprint("celsius"),
                             tracks::DimensionBlueprints(),
                             tracks::StaticNameBlueprint("BatteryTemperature"));

constexpr auto kBatteryHealthBlueprint =
    tracks::CounterBlueprint("battery_health",
                             tracks::UnknownUnitBlueprint(),
                             tracks::DimensionBlueprints(),
                             tracks::StaticNameBlueprint("BatteryHealth"));

// Charge drawn through the modem and wifi power rails, if the device can
// measure it.
constexpr auto kRailChargeBlueprint = tracks::CounterBlueprint(
    "battery_rail_charge",
    tracks::StaticUnitBlueprint("mAh"),
    tracks::DimensionBlueprints(tracks::StringDimensionBlueprint("rail")),
    tracks::FnNameBlueprint([](base::StringView rail) {
      return base::StackString<255>("%.*sRailCharge", int(rail.size()),
                                    rail.data());
    }));

// Slices for battery stats events and wakeup reasons, one track per kind.
constexpr auto kBatteryStatsEventBlueprint = tracks::SliceBlueprint(
    "battery_stats",
    tracks::Dimensions(tracks::StringDimensionBlueprint("bstats_item_name")),
    tracks::FnNameBlueprint([](base::StringView item) {
      return base::StackString<1024>("battery_stats.%.*s", int(item.size()),
                                     item.data());
    }));

}  // namespace

AndroidDumpstateEventParser::~AndroidDumpstateEventParser() = default;

void AndroidDumpstateEventParser::Parse(int64_t ts,
                                        AndroidDumpstateEvent event) {
  switch (event.type) {
    case AndroidDumpstateEvent::EventType::kBatteryStatsHistoryEvent:
      if (!ProcessBatteryStatsHistoryItem(ts, event.raw_event).ok()) {
        context_->stats_tracker->IncrementStats(
            stats::android_battery_stats_unhandled_items);
      }
      return;
    case AndroidDumpstateEvent::EventType::kNull:
      return;
  }
}

std::optional<AndroidDumpstateEventParser::HistoryTag>
AndroidDumpstateEventParser::ResolveHistoryTag(base::StringView value) {
  // Usually the value is an index into the history string pool.
  std::optional<int64_t> hsp_index = base::StringViewToInt64(value);
  if (hsp_index.has_value()) {
    if (!history_string_tracker_->HasIndex(*hsp_index)) {
      return std::nullopt;
    }
    return HistoryTag{history_string_tracker_->GetUid(*hsp_index),
                      history_string_tracker_->GetString(*hsp_index)};
  }
  // If the string pool overflowed, the tag is inlined as uid:"string", with
  // quotes in the string doubled.
  size_t colon = value.find(':');
  if (colon == base::StringView::npos) {
    return std::nullopt;
  }
  std::optional<int32_t> uid = ParseUid(value.substr(0, colon));
  base::StringView str = value.substr(colon + 1);
  if (!uid.has_value() || str.size() < 2 || str.at(0) != '"' ||
      str.at(str.size() - 1) != '"') {
    return std::nullopt;
  }
  return HistoryTag{
      *uid, base::ReplaceAll(str.substr(1, str.size() - 2).ToStdString(),
                             "\"\"", "\"")};
}

base::Status AndroidDumpstateEventParser::ProcessBatteryStatsHistoryItem(
    int64_t ts,
    const std::string& raw_event) {
  base::StringView raw(raw_event);
  size_t eq = raw.find('=');
  TokenizedBatteryStatsHistoryItem item;
  item.ts = ts;
  item.key = raw.substr(0, eq);
  // Everything after the first '=': inlined strings may contain '='.
  item.value =
      eq == base::StringView::npos ? base::StringView() : raw.substr(eq + 1);
  item.prefix = "";
  if (item.key.size() > 0 && (item.key.at(0) == '+' || item.key.at(0) == '-')) {
    item.prefix = item.key.substr(0, 1);
    item.key = item.key.substr(1);
  }

  // Attempt to parse the input with each sub-parser until we find one that can
  // Successfully parse the event.
  bool parsed = false;

  // Attempt to parse the the event as a battery stats Event ("E" prefix)
  ASSIGN_OR_RETURN(parsed, ProcessBatteryStatsHistoryEvent(item));
  if (parsed) {
    return base::OkStatus();
  }

  // Attempt to parse the the event as a battery stats state
  ASSIGN_OR_RETURN(parsed, ProcessBatteryStatsHistoryState(item));
  if (parsed) {
    return base::OkStatus();
  }

  // Attempt to parse the event as a battery stats battery counter
  ASSIGN_OR_RETURN(parsed, ProcessBatteryStatsHistoryBatteryCounter(item));
  if (parsed) {
    return base::OkStatus();
  }

  // Attempt to parse any wakelock events (eg. +w=123)
  ASSIGN_OR_RETURN(parsed, ProcessBatteryStatsHistoryWakeLocks(item));
  if (parsed) {
    return base::OkStatus();
  }

  // Attempt to parse kernel wakeup reasons (eg. wr=123)
  ASSIGN_OR_RETURN(parsed, ProcessBatteryStatsHistoryWakeupReason(item));
  if (parsed) {
    return base::OkStatus();
  }

  return base::ErrStatus("Unhandled battery stats event");
}

base::StatusOr<bool>
AndroidDumpstateEventParser::ProcessBatteryStatsHistoryEvent(
    const TokenizedBatteryStatsHistoryItem& item) {
  if (!item.key.StartsWith("E")) {
    return false;
  }
  // Process a history event
  ASSIGN_OR_RETURN(std::string item_name, GetEventFromShortName(item.key));
  std::optional<HistoryTag> tag = ResolveHistoryTag(item.value);
  if (!tag.has_value()) {
    return base::ErrStatus("Failed to resolve battery stats event tag");
  }

  base::StackString<255> slice_name(
      "%s%s=%d:\"%s\"", item.prefix.ToStdString().c_str(), item_name.c_str(),
      tag->uid, tag->str.c_str());
  StringId name_id = context_->storage->InternString(slice_name.c_str());
  TrackId track_id = context_->track_tracker->InternTrack(
      kBatteryStatsEventBlueprint,
      tracks::Dimensions(base::StringView(item_name)));
  context_->slice_tracker->Scoped(item.ts, track_id, kNullStringId, name_id, 0);
  return true;
}

base::StatusOr<bool>
AndroidDumpstateEventParser::ProcessBatteryStatsHistoryWakeupReason(
    const TokenizedBatteryStatsHistoryItem& item) {
  if (!item.prefix.empty() || item.key != "wr" || item.value.empty()) {
    return false;
  }
  // The reason the kernel woke the CPU up, recorded when the CPU starts
  // running (+r). It is an instant, named like events: wakeup_reason=0:"...".
  std::optional<HistoryTag> tag = ResolveHistoryTag(item.value);
  if (!tag.has_value()) {
    return base::ErrStatus("Failed to resolve wakeup reason tag");
  }
  base::StackString<255> slice_name("wakeup_reason=%d:\"%s\"", tag->uid,
                                    tag->str.c_str());
  StringId name_id = context_->storage->InternString(slice_name.c_str());
  TrackId track_id = context_->track_tracker->InternTrack(
      kBatteryStatsEventBlueprint,
      tracks::Dimensions(base::StringView("wakeup_reason")));
  context_->slice_tracker->Scoped(item.ts, track_id, kNullStringId, name_id, 0);
  return true;
}

base::StatusOr<bool>
AndroidDumpstateEventParser::ProcessBatteryStatsHistoryState(
    const TokenizedBatteryStatsHistoryItem& item) {
  // Process a history state of the form "+state" or "-state"
  if (!item.prefix.empty() && item.value.empty()) {
    // To match behavior of the battery stats atrace implementation, avoid
    // including Wakelock events in the trace as counters.
    if (item.key == "w") {
      return true;
    }

    ASSIGN_OR_RETURN(std::string item_name,
                     GetStateAndValueFromShortName(item.key, "", nullptr));
    TrackId track = context_->track_tracker->InternTrack(
        tracks::kAndroidBatteryStatsBlueprint,
        tracks::Dimensions(
            base::StringView(std::string("battery_stats.").append(item_name))));
    context_->event_tracker->PushCounter(
        item.ts, (item.prefix == "+") ? 1.0 : 0.0, track);
    // Also add a screen events to the screen state track
    if (item_name == "screen") {
      track = context_->track_tracker->InternTrack(
          tracks::kAndroidScreenStateBlueprint);
      // battery_stats.screen event is 0 for off and 1 for on, but the
      // ScreenState track uses the convention 1 for off and 2 for on, so add
      // 1 to the current counter value.
      context_->event_tracker->PushCounter(
          item.ts, static_cast<double>((item.prefix == "+") ? 2.0 : 1.0),
          track);
    }

    return true;
  } else if (item.prefix.empty() && !item.value.empty()) {
    int64_t counter_value;
    base::StatusOr<std::string> possible_history_state_item =
        GetStateAndValueFromShortName(item.key, item.value, &counter_value);
    if (possible_history_state_item.ok()) {
      std::string item_name = possible_history_state_item.value();
      TrackId counter_track = context_->track_tracker->InternTrack(
          tracks::kAndroidBatteryStatsBlueprint,
          tracks::Dimensions(base::StringView(
              std::string("battery_stats.").append(item_name))));
      context_->event_tracker->PushCounter(
          item.ts, static_cast<double>(counter_value), counter_track);
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

base::StatusOr<bool>
AndroidDumpstateEventParser::ProcessBatteryStatsHistoryBatteryCounter(
    const TokenizedBatteryStatsHistoryItem& item) {
  if (!item.prefix.empty() || item.value.empty() ||
      !(item.key.StartsWith("B") || item.key == "Mrc" || item.key == "Wrc")) {
    return false;
  }
  // AndroidProbesParser will use the empty string for the battery name if no
  // battery name is associated with the data, which is common on most pixel
  // phones. Adopt the same convention here. Battery stats does not provide
  // a battery name in the checking format, so we'll always have an unknown
  // battery.
  const base::StringView kUnknownBatteryName = "";

  // process history state of form "state=12345" or "state=abcde"
  TrackId counter_track;
  double counter_value;
  if (item.key == "Bl") {
    counter_track = context_->track_tracker->InternTrack(
        tracks::kBatteryCounterBlueprint,
        tracks::Dimensions(kUnknownBatteryName, "capacity_pct"));
    ASSIGN_OR_RETURN(int64_t level, StringToStatusOrInt64(item.value));
    counter_value = static_cast<double>(level);
  } else if (item.key == "Bcc") {
    counter_track = context_->track_tracker->InternTrack(
        tracks::kBatteryCounterBlueprint,
        tracks::Dimensions(kUnknownBatteryName, "charge_uah"));
    ASSIGN_OR_RETURN(int64_t charge_mah, StringToStatusOrInt64(item.value));
    // battery stats gives us charge in milli-amp-hours, but the track
    // expects the value to be in micro-amp-hours
    counter_value = static_cast<double>(charge_mah * 1000);
  } else if (item.key == "Bv") {
    counter_track = context_->track_tracker->InternTrack(
        tracks::kBatteryCounterBlueprint,
        tracks::Dimensions(kUnknownBatteryName, "voltage_uv"));
    ASSIGN_OR_RETURN(int64_t voltage_mv, StringToStatusOrInt64(item.value));
    // battery stats gives us charge in milli-volts, but the track
    // expects the value to be in micro-volts
    counter_value = static_cast<double>(voltage_mv * 1000);
  } else if (item.key == "Bs") {
    static constexpr auto kBatteryStatusBlueprint = tracks::CounterBlueprint(
        "battery_status", tracks::UnknownUnitBlueprint(),
        tracks::DimensionBlueprints(),
        tracks::StaticNameBlueprint("BatteryStatus"));
    counter_track =
        context_->track_tracker->InternTrack(kBatteryStatusBlueprint);
    switch (item.value.at(0)) {
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
        PERFETTO_ELOG("unknown battery status: %c", item.value.at(0));
        counter_value = 0;  // not a valid enum
    }
  } else if (item.key == "Bp") {
    static constexpr auto kPluggedStatusBluePrint = tracks::CounterBlueprint(
        "battery_plugged_status", tracks::UnknownUnitBlueprint(),
        tracks::DimensionBlueprints(), tracks::StaticNameBlueprint("PlugType"));
    counter_track =
        context_->track_tracker->InternTrack(kPluggedStatusBluePrint);
    switch (item.value.at(0)) {
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
      case 'd':
        counter_value = 8;  // BatteryManager.BATTERY_PLUGGED_DOCK
        break;
      default:
        counter_value = 0;  // BatteryManager.BATTERY_PLUGGED_NONE
    }
  } else if (item.key == "Bt") {
    counter_track =
        context_->track_tracker->InternTrack(kBatteryTemperatureBlueprint);
    ASSIGN_OR_RETURN(int64_t deci_celsius, StringToStatusOrInt64(item.value));
    counter_value = static_cast<double>(deci_celsius) / 10.0;
  } else if (item.key == "Bh") {
    counter_track =
        context_->track_tracker->InternTrack(kBatteryHealthBlueprint);
    switch (item.value.at(0)) {
      case '?':
        counter_value = 1;  // BatteryManager.BATTERY_HEALTH_UNKNOWN
        break;
      case 'g':
        counter_value = 2;  // BatteryManager.BATTERY_HEALTH_GOOD
        break;
      case 'h':
        counter_value = 3;  // BatteryManager.BATTERY_HEALTH_OVERHEAT
        break;
      case 'd':
        counter_value = 4;  // BatteryManager.BATTERY_HEALTH_DEAD
        break;
      case 'v':
        counter_value = 5;  // BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE
        break;
      case 'f':
        // BatteryManager.BATTERY_HEALTH_UNSPECIFIED_FAILURE
        counter_value = 6;
        break;
      case 'c':
        counter_value = 7;  // BatteryManager.BATTERY_HEALTH_COLD
        break;
      default: {
        // Values without a short name are printed as numbers.
        ASSIGN_OR_RETURN(int64_t health, StringToStatusOrInt64(item.value));
        counter_value = static_cast<double>(health);
      }
    }
  } else if (item.key == "Mrc" || item.key == "Wrc") {
    std::optional<double> charge_mah =
        base::StringToDouble(item.value.ToStdString());
    if (!charge_mah.has_value()) {
      return base::ErrStatus("Failed to parse rail charge");
    }
    counter_track = context_->track_tracker->InternTrack(
        kRailChargeBlueprint, tracks::Dimensions(base::StringView(
                                  item.key == "Mrc" ? "Modem" : "Wifi")));
    counter_value = *charge_mah;
  } else {
    return false;
  }

  context_->event_tracker->PushCounter(item.ts, counter_value, counter_track);
  return true;
}

base::StatusOr<bool>
AndroidDumpstateEventParser::ProcessBatteryStatsHistoryWakeLocks(
    const TokenizedBatteryStatsHistoryItem& item) {
  if (item.prefix.empty() || item.key != "w" || item.value.empty()) {
    return false;
  }
  // We can only support wakeup parsing on battery stats ver 36+ since on
  // older versions the "-w" event does not have a history string
  // associated with it. This history sting is needed, since we use the
  // HSP index as the "cookie" to disambiguate overlapping wakelocks.
  if (history_string_tracker_->battery_stats_version() < 36) {
    return base::ErrStatus("Wakelocks unsupported on batterystats ver < 36");
  }

  static constexpr auto kBlueprint = TrackCompressor::SliceBlueprint(
      "dumpstate_wakelocks", tracks::DimensionBlueprints(),
      tracks::StaticNameBlueprint("WakeLocks"));

  ASSIGN_OR_RETURN(int64_t hsp_index, StringToStatusOrInt64(item.value));
  if (item.prefix == "+") {
    StringId name_id = context_->storage->InternString(
        history_string_tracker_->GetString(hsp_index));
    TrackId id = context_->track_compressor->InternBegin(
        kBlueprint, tracks::Dimensions(), static_cast<int64_t>(hsp_index));
    context_->slice_tracker->Begin(item.ts, id, kNullStringId, name_id);
  } else {
    TrackId id = context_->track_compressor->InternEnd(
        kBlueprint, tracks::Dimensions(), static_cast<int64_t>(hsp_index));
    context_->slice_tracker->End(item.ts, id);
  }
  return true;
}

}  // namespace perfetto::trace_processor
