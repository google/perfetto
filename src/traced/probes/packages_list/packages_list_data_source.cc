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

#include "src/traced/probes/packages_list/packages_list_data_source.h"

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/string_splitter.h"

#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing/core/trace_writer.h"

namespace perfetto {

bool ReadPackagesListLine(char* line, Package* package) {
  size_t idx = 0;
  for (base::StringSplitter ss(line, ' '); ss.Next();) {
    switch (idx) {
      case 0:
        package->name = std::string(ss.cur_token(), ss.cur_token_size());
        break;
      case 1: {
        char* end;
        long long uid = strtoll(ss.cur_token(), &end, 10);
        if ((*end != '\0' && *end != '\n') || *ss.cur_token() == '\0') {
          PERFETTO_ELOG("Failed to parse packages.list uid.");
          return false;
        }
        package->uid = static_cast<uint64_t>(uid);
        break;
      }
      case 2: {
        char* end;
        long long debuggable = strtoll(ss.cur_token(), &end, 10);
        if ((*end != '\0' && *end != '\n') || *ss.cur_token() == '\0') {
          PERFETTO_ELOG("Failed to parse packages.list debuggable.");
          return false;
        }
        package->debuggable = debuggable != 0;
        break;
      }
      case 6: {
        char* end;
        long long profilable_from_shell = strtoll(ss.cur_token(), &end, 10);
        if ((*end != '\0' && *end != '\n') || *ss.cur_token() == '\0') {
          PERFETTO_ELOG("Failed to parse packages.list profilable_from_shell.");
          return false;
        }
        package->profileable_from_shell = profilable_from_shell != 0;
        break;
      }
      case 7: {
        char* end;
        long long version_code = strtoll(ss.cur_token(), &end, 10);
        if ((*end != '\0' && *end != '\n') || *ss.cur_token() == '\0') {
          PERFETTO_ELOG("Failed to parse packages.list version_code: %s.",
                        ss.cur_token());
          return false;
        }
        package->version_code = version_code;
        break;
      }
    }
    ++idx;
  }
  return true;
}

PackagesListDataSource::PackagesListDataSource(
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, kTypeId), writer_(std::move(writer)) {}

void PackagesListDataSource::Start() {
  base::ScopedFstream fs(fopen("/data/system/packages.list", "r"));
  auto trace_packet = writer_->NewTracePacket();
  auto* packages_list_packet = trace_packet->set_packages_list();
  if (!fs) {
    PERFETTO_ELOG("Failed to open packages.list");
    packages_list_packet->set_error(true);
    trace_packet->Finalize();
    writer_->Flush();
    return;
  }
  char line[2048];
  while (fgets(line, sizeof(line), *fs) != nullptr) {
    Package pkg_struct;
    if (ReadPackagesListLine(line, &pkg_struct)) {
      auto* package = packages_list_packet->add_packages();
      package->set_name(pkg_struct.name.c_str(), pkg_struct.name.size());
      package->set_uid(pkg_struct.uid);
      package->set_debuggable(pkg_struct.debuggable);
      package->set_profileable_from_shell(pkg_struct.profileable_from_shell);
      package->set_version_code(pkg_struct.version_code);
    } else {
      packages_list_packet->set_error(true);
    }
  }
  trace_packet->Finalize();
  writer_->Flush();
}

void PackagesListDataSource::Flush(FlushRequestID,
                                   std::function<void()> callback) {
  // Flush is no-op. We flush after the first write.
  callback();
}

PackagesListDataSource::~PackagesListDataSource() = default;

}  // namespace perfetto
