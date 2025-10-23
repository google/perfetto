/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/perfetto_cmd/config.h"

#include <stdlib.h>
#include <string_view>
#include <array>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/trace_config.h"

#include "protos/perfetto/config/ftrace/ftrace_config.gen.h"
#include "protos/perfetto/config/sys_stats/sys_stats_config.gen.h"

namespace perfetto {
namespace {

// Use constexpr for compile-time constants
constexpr uint32_t kDefaultFlushPeriodMs = 30'000;
constexpr uint32_t kDiskStatPeriodMs = 1'000;
constexpr uint64_t kBytesPerKb = 1'024;

// Use std::pair with const char* to avoid string allocations
using UnitMultiplier = std::pair<std::string_view, uint64_t>;

// Optimized: Use string_view to avoid unnecessary string copies
struct ValueUnit {
  uint64_t value;
  std::string_view unit;
};

// Optimized: Use string_view parameter, more efficient parsing
[[nodiscard]] bool SplitValueAndUnit(std::string_view arg, ValueUnit& out) {
  if (arg.empty()) return false;

  // Find where digits end
  char* end;
  out.value = strtoull(arg.data(), &end, 10);
  
  if (end == arg.data()) return false;
  
  // Extract unit as string_view (zero-copy)
  const size_t consumed = static_cast<size_t>(end - arg.data());
  out.unit = arg.substr(consumed);
  
  return true;
}

// Optimized: Use constexpr array and string_view for zero-allocation lookup
template<size_t N>
[[nodiscard]] bool ConvertValue(std::string_view arg,
                                const std::array<UnitMultiplier, N>& units,
                                uint64_t& out) {
  if (arg.empty() || arg == "0") {
    out = 0;
    return true;
  }

  ValueUnit value_unit{};
  if (!SplitValueAndUnit(arg, value_unit)) return false;

  // Linear search is fine for small arrays (< 10 elements)
  for (const auto& [unit, multiplier] : units) {
    if (value_unit.unit == unit) {
      // Check for overflow
      if (value_unit.value > UINT64_MAX / multiplier) {
        return false;
      }
      out = value_unit.value * multiplier;
      return true;
    }
  }
  return false;
}

// Optimized: Use constexpr arrays defined at compile time
[[nodiscard]] bool ConvertTimeToMs(std::string_view arg, uint64_t& out) {
  constexpr std::array<UnitMultiplier, 4> kTimeUnits = {{
      {"ms", 1},
      {"s", 1'000},
      {"m", 60'000},
      {"h", 3'600'000},
  }};
  return ConvertValue(arg, kTimeUnits, out);
}

[[nodiscard]] bool ConvertSizeToKb(std::string_view arg, uint64_t& out) {
  constexpr std::array<UnitMultiplier, 6> kSizeUnits = {{
      {"kb", 1},
      {"mb", 1'024},
      {"gb", 1'048'576},
      {"k", 1},
      {"m", 1'024},
      {"g", 1'048'576},
  }};
  return ConvertValue(arg, kSizeUnits, out);
}

// Optimized: Extract data source creation to reduce duplication
void AddDataSource(TraceConfig* config, std::string_view name, 
                   uint32_t target_buffer = 0, 
                   const std::string* raw_config = nullptr) {
  auto* ds = config->add_data_sources()->mutable_config();
  ds->set_name(std::string(name));
  if (target_buffer > 0) {
    ds->set_target_buffer(target_buffer);
  }
  if (raw_config) {
    ds->set_ftrace_config_raw(*raw_config);
  }
}

// Optimized: Extracted category-specific logic
void HandleGfxCategory(TraceConfig* config) {
  AddDataSource(config, "android.surfaceflinger.frametimeline");
}

void HandleDiskCategory(TraceConfig* config) {
  protos::gen::SysStatsConfig cfg;
  cfg.set_diskstat_period_ms(kDiskStatPeriodMs);
  
  auto* ds = config->add_data_sources()->mutable_config();
  ds->set_name("linux.sys_stats");
  ds->set_sys_stats_config_raw(cfg.SerializeAsString());
}

void AddFtraceDataSource(TraceConfig* config,
                        const std::vector<std::string>& ftrace_events,
                        const std::vector<std::string>& atrace_categories,
                        const std::vector<std::string>& atrace_apps) {
  if (ftrace_events.empty() && atrace_categories.empty() && atrace_apps.empty()) {
    return;
  }

  protos::gen::FtraceConfig ftrace_cfg;
  
  // Reserve space to avoid reallocations
  if (!ftrace_events.empty()) {
    for (const auto& evt : ftrace_events) {
      ftrace_cfg.add_ftrace_events(evt);
    }
  }
  
  if (!atrace_categories.empty()) {
    for (const auto& cat : atrace_categories) {
      ftrace_cfg.add_atrace_categories(cat);
    }
  }
  
  if (!atrace_apps.empty()) {
    for (const auto& app : atrace_apps) {
      ftrace_cfg.add_atrace_apps(app);
    }
  }
  
  ftrace_cfg.set_symbolize_ksyms(true);
  
  auto* ds = config->add_data_sources()->mutable_config();
  ds->set_name("linux.ftrace");
  ds->set_ftrace_config_raw(ftrace_cfg.SerializeAsString());
}

void AddHypervisorDataSource(TraceConfig* config, std::string_view hyp_category) {
  protos::gen::FtraceConfig ftrace_cfg;
  ftrace_cfg.set_instance_name(std::string(hyp_category));
  ftrace_cfg.add_ftrace_events(std::string(hyp_category) + "/*");
  
  auto* ds = config->add_data_sources()->mutable_config();
  ds->set_name("linux.ftrace");
  ds->set_ftrace_config_raw(ftrace_cfg.SerializeAsString());
}

}  // namespace

bool CreateConfigFromOptions(const ConfigOptions& options,
                             TraceConfig* config) {
  // Validate and convert time
  uint64_t duration_ms = 0;
  if (!ConvertTimeToMs(options.time, duration_ms)) {
    PERFETTO_ELOG("--time argument is invalid: '%s'", options.time.c_str());
    return false;
  }

  // Validate and convert buffer size
  uint64_t buffer_size_kb = 0;
  if (!ConvertSizeToKb(options.buffer_size, buffer_size_kb)) {
    PERFETTO_ELOG("--buffer argument is invalid: '%s'", 
                  options.buffer_size.c_str());
    return false;
  }

  // Validate and convert max file size
  uint64_t max_file_size_kb = 0;
  if (!ConvertSizeToKb(options.max_file_size, max_file_size_kb)) {
    PERFETTO_ELOG("--size argument is invalid: '%s'", 
                  options.max_file_size.c_str());
    return false;
  }

  // Categorize inputs
  std::vector<std::string> ftrace_events;
  std::vector<std::string> atrace_categories;
  std::optional<std::string> hyp_category;

  // Reserve space for better performance
  ftrace_events.reserve(options.categories.size());
  atrace_categories.reserve(options.categories.size());

  for (const auto& category : options.categories) {
    // Check for ftrace events (contain '/')
    if (base::Contains(category, '/')) {
      ftrace_events.push_back(category);
    } 
    // Check for hypervisor category
    else if (category == "hyp" || category == "hypervisor") {
      hyp_category = category;
    } 
    // Otherwise it's an atrace category
    else {
      atrace_categories.push_back(category);
    }

    // Handle special categories
    if (category == "gfx") {
      HandleGfxCategory(config);
    } else if (category == "disk") {
      HandleDiskCategory(config);
    }
  }

  // Configure trace settings
  config->set_duration_ms(static_cast<uint32_t>(duration_ms));
  config->set_max_file_size_bytes(max_file_size_kb * kBytesPerKb);
  config->set_flush_period_ms(kDefaultFlushPeriodMs);
  
  if (max_file_size_kb > 0) {
    config->set_write_into_file(true);
  }
  
  config->add_buffers()->set_size_kb(static_cast<uint32_t>(buffer_size_kb));

  // Add data sources
  AddFtraceDataSource(config, ftrace_events, atrace_categories, 
                      options.atrace_apps);

  if (hyp_category.has_value()) {
    AddHypervisorDataSource(config, *hyp_category);
  }

  // Add standard data sources
  AddDataSource(config, "linux.process_stats", 0);
  AddDataSource(config, "linux.system_info", 0);

  return true;
}

}  // namespace perfetto