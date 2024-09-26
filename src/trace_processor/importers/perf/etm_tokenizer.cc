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

#include "src/trace_processor/importers/perf/etm_tokenizer.h"
#include <memory>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_data_tokenizer.h"
#include "src/trace_processor/importers/perf/aux_record.h"
#include "src/trace_processor/importers/perf/perf_session.h"
#include "src/trace_processor/importers/perf/reader.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

struct EtmV4Info {
  uint64_t cpu;
  uint64_t nrtrcparams;
  uint64_t trcconfigr;
  uint64_t trctraceidr;
  uint64_t trcidr0;
  uint64_t trcidr1;
  uint64_t trcidr2;
  uint64_t trcidr8;
  uint64_t trcauthstatus;
};

struct EteInfo : public EtmV4Info {
  uint64_t trcdevarch;
};

struct EtmConfiguration {
  util::Status Parse(TraceBlobView data);
  uint64_t version;
  uint32_t pmu_type;
  uint64_t snapshot;
  std::vector<EtmV4Info> etm_v4_infos;
  std::vector<EteInfo> ete_infos;
};

util::Status EtmConfiguration::Parse(TraceBlobView data) {
  static constexpr uint64_t kEtmV4Magic = 0x4040404040404040ULL;
  static constexpr uint64_t kEteMagic = 0x5050505050505050ULL;
  Reader reader(std::move(data));

  if (!reader.Read(version)) {
    return base::ErrStatus("Failed to parse EtmConfiguration.");
  }

  if (version != 1) {
    return base::ErrStatus("Invalid version in EtmConfiguration: %" PRIu64,
                           version);
  }

  uint32_t nr;
  if (!reader.Read(nr) || !reader.Read(pmu_type) || !reader.Read(snapshot)) {
    return base::ErrStatus("Failed to parse EtmConfiguration.");
  }

  for (; nr != 0; --nr) {
    uint64_t magic;
    if (!reader.Read(magic)) {
      return base::ErrStatus("Failed to parse EtmConfiguration.");
    }
    switch (magic) {
      case kEtmV4Magic:
        etm_v4_infos.emplace_back();
        if (!reader.Read(etm_v4_infos.back())) {
          return base::ErrStatus("Failed to parse EtmV4Info.");
        }
        break;
      case kEteMagic:
        ete_infos.emplace_back();
        if (!reader.Read(ete_infos.back())) {
          return base::ErrStatus("Failed to parse EteInfo.");
        }
        break;
      default:
        return base::ErrStatus("Unknown magic in EtmConfiguration: %s",
                               base::Uint64ToHexString(magic).c_str());
    }
  }

  return base::OkStatus();
}

}  // namespace

base::StatusOr<std::unique_ptr<AuxDataTokenizerFactory>>
CreateEtmTokenizerFactory(TraceBlobView data) {
  EtmConfiguration config;
  RETURN_IF_ERROR(config.Parse(std::move(data)));
  return std::unique_ptr<AuxDataTokenizerFactory>(
      new DummyAuxDataTokenizerFactory());
}

}  // namespace perfetto::trace_processor::perf_importer
