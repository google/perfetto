/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/traced/probes/system_info/system_info_data_source.h"

#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/cpu_info.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/system_info/system_info_config.gen.h"
#include "protos/perfetto/trace/system_info/cpu_info.pbzero.h"
#include "protos/perfetto/trace/system_info/interrupt_info.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

struct InterruptMapping {
  int32_t irq_id;
  std::string name;
};

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
std::vector<InterruptMapping> ReadInterruptMappings() {
  std::vector<InterruptMapping> mappings;
  std::string content;
  if (!base::ReadFile("/proc/interrupts", &content))
    return mappings;

  base::StringSplitter lines(std::move(content), '\n');
  if (!lines.Next())
    return mappings;

  // 1. Determine number of CPUs from the header line.
  size_t num_cpus = 0;
  {
    std::string header = lines.cur_token();
    std::string current_token;
    for (char c : header) {
      if (c == ' ' || c == '\t') {
        if (!current_token.empty()) {
          if (base::StartsWith(current_token, "CPU"))
            num_cpus++;
          current_token.clear();
        }
      } else {
        current_token += c;
      }
    }
    if (!current_token.empty() && base::StartsWith(current_token, "CPU")) {
      num_cpus++;
    }
  }

  if (num_cpus == 0)
    return mappings;

  // 2. Parse data lines.
  while (lines.Next()) {
    std::string line = lines.cur_token();
    std::vector<std::string> tokens;
    std::string current_token;
    for (char c : line) {
      if (c == ' ' || c == '\t') {
        if (!current_token.empty()) {
          tokens.push_back(std::move(current_token));
          current_token.clear();
        }
      } else {
        current_token += c;
      }
    }
    if (!current_token.empty()) {
      tokens.push_back(std::move(current_token));
    }

    if (tokens.size() <= num_cpus)
      continue;

    // The first token must be "IRQ_NUM:".
    const std::string& id_token = tokens[0];
    char* endptr;
    int32_t irq_id =
        static_cast<int32_t>(strtoll(id_token.c_str(), &endptr, 10));
    if (endptr == id_token.c_str() || *endptr != ':')
      continue;

    // Search for "Level" or "Edge" as an anchor for the name.
    ssize_t trigger_index = -1;
    for (size_t i = num_cpus + 1; i < tokens.size(); ++i) {
      if (tokens[i] == "Level" || tokens[i] == "Edge") {
        trigger_index = static_cast<ssize_t>(i);
        break;
      }
    }

    size_t name_start_index;
    if (trigger_index != -1) {
      // Name starts immediately after the trigger.
      name_start_index = static_cast<size_t>(trigger_index + 1);
    } else {
      // Fallback: Skip IRQ ID (1), CPU counts, and 2 metadata fields.
      name_start_index = num_cpus + 3;
    }

    if (name_start_index >= tokens.size())
      continue;

    InterruptMapping mapping;
    mapping.irq_id = irq_id;
    for (size_t j = name_start_index; j < tokens.size(); ++j) {
      if (!mapping.name.empty())
        mapping.name += " ";
      mapping.name += tokens[j];
    }

    if (!mapping.name.empty()) {
      mappings.push_back(std::move(mapping));
    }
  }

  return mappings;
}
#endif

}  // namespace

// static
const ProbesDataSource::Descriptor SystemInfoDataSource::descriptor = {
    /* name */ "linux.system_info",
    /* flags */ Descriptor::kFlagsNone,
    /* fill_descriptor_func */ nullptr,
};

SystemInfoDataSource::SystemInfoDataSource(
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer,
    std::unique_ptr<CpuFreqInfo> cpu_freq_info,
    const DataSourceConfig& config)
    : ProbesDataSource(session_id, &descriptor),
      writer_(std::move(writer)),
      cpu_freq_info_(std::move(cpu_freq_info)) {
  include_irq_mapping_ = config.system_info_config().irq_names();
}

void SystemInfoDataSource::Start() {
  auto packet = writer_->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* cpu_info = packet->set_cpu_info();

  for (const auto& parsed_cpu : ReadCpuInfo()) {
    auto* cpu = cpu_info->add_cpus();
    cpu->set_processor(parsed_cpu.processor);

    std::optional<uint32_t> cpu_capacity =
        base::StringToUInt32(base::StripSuffix(
            ReadFile("/sys/devices/system/cpu/cpu" +
                     std::to_string(parsed_cpu.cpu_index) + "/cpu_capacity"),
            "\n"));

    if (cpu_capacity.has_value()) {
      cpu->set_capacity(cpu_capacity.value());
    }

    auto freqs_range = cpu_freq_info_->GetFreqs(parsed_cpu.cpu_index);
    for (auto it = freqs_range.first; it != freqs_range.second; it++) {
      cpu->add_frequencies(*it);
    }

    if (parsed_cpu.implementer && parsed_cpu.architecture && parsed_cpu.part &&
        parsed_cpu.variant && parsed_cpu.revision) {
      auto* identifier = cpu->set_arm_identifier();
      identifier->set_implementer(parsed_cpu.implementer.value());
      identifier->set_architecture(parsed_cpu.architecture.value());
      identifier->set_variant(parsed_cpu.variant.value());
      identifier->set_part(parsed_cpu.part.value());
      identifier->set_revision(parsed_cpu.revision.value());
    } else if (parsed_cpu.implementer || parsed_cpu.architecture ||
               parsed_cpu.part || parsed_cpu.variant || parsed_cpu.revision) {
      PERFETTO_DLOG("Arm specific fields not found for cpu %" PRIu32,
                    parsed_cpu.cpu_index);
    }

    if (parsed_cpu.features != 0) {
      cpu->set_features(parsed_cpu.features);
    }
  }

  packet->Finalize();

  if (include_irq_mapping_) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    auto mappings = ReadInterruptMappings();
    if (!mappings.empty()) {
      auto irq_packet = writer_->NewTracePacket();
      irq_packet->set_timestamp(
          static_cast<uint64_t>(base::GetBootTimeNs().count()));
      auto* interrupt_info = irq_packet->set_interrupt_info();
      for (const auto& mapping : mappings) {
        auto* irq_proto = interrupt_info->add_irq_mapping();
        irq_proto->set_irq_id(mapping.irq_id);
        irq_proto->set_name(mapping.name);
      }
    }
#endif
  }
  writer_->Flush();
}

void SystemInfoDataSource::Flush(FlushRequestID,
                                 std::function<void()> callback) {
  writer_->Flush(callback);
}

std::vector<base::CpuInfo> SystemInfoDataSource::ReadCpuInfo() {
  return base::ReadCpuInfo();
}

std::string SystemInfoDataSource::ReadFile(std::string path) {
  std::string contents;
  if (!base::ReadFile(path, &contents))
    return "";
  return contents;
}

}  // namespace perfetto
