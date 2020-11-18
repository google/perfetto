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

#ifndef SRC_PROFILING_COMMON_PROFILER_GUARDRAILS_H_
#define SRC_PROFILING_COMMON_PROFILER_GUARDRAILS_H_

#include <inttypes.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "src/profiling/common/proc_utils.h"

namespace perfetto {
namespace profiling {

class ProfilerCpuGuardrails {
 public:
  explicit ProfilerCpuGuardrails(base::ScopedFile stat_fd)
      : stat_fd_(std::move(stat_fd)) {}

  template <typename T, typename F>
  void CheckDataSourceCpu(T begin, T end, F guardrail_hit_callback) {
    bool any_guardrail = false;
    for (auto it = begin; it != end; ++it) {
      auto& ds = it->second;
      if (ds.GetCpuGuardrailSecs() > 0) {
        any_guardrail = true;
        break;
      }
    }
    if (!any_guardrail)
      return;

    base::Optional<uint64_t> opt_cputime_sec = GetCputimeSec();
    if (!opt_cputime_sec) {
      PERFETTO_ELOG("Failed to get CPU time.");
      return;
    }

    uint64_t cputime_sec = *opt_cputime_sec;

    for (auto it = begin; it != end; ++it) {
      auto& ds = it->second;
      uint64_t ds_max_cpu = ds.GetCpuGuardrailSecs();
      if (ds_max_cpu > 0) {
        auto start_cputime_sec = ds.GetCpuStartSecs();
        // We reject data-sources with CPU guardrails if we cannot read the
        // initial value, which means we get a non-nullopt value here.
        PERFETTO_CHECK(start_cputime_sec);
        uint64_t cpu_diff = cputime_sec - *start_cputime_sec;
        if (cputime_sec > *start_cputime_sec && cpu_diff > ds_max_cpu) {
          PERFETTO_ELOG(
              "Exceeded data-source CPU guardrail "
              "(%" PRIu64 " > %" PRIu64 "). Shutting down.",
              cpu_diff, ds_max_cpu);
          guardrail_hit_callback(&ds);
        }
      }
    }
  }

  base::Optional<uint64_t> GetCputimeSec();

 private:
  base::ScopedFile stat_fd_;
};

class ProfilerMemoryGuardrails {
 public:
  explicit ProfilerMemoryGuardrails(base::ScopedFile status_fd)
      : status_fd_(std::move(status_fd)) {}

  template <typename T, typename F>
  void CheckDataSourceMemory(T begin, T end, F guardrail_hit_callback) {
    bool any_guardrail = false;
    for (auto it = begin; it != end; ++it) {
      auto& ds = it->second;
      if (ds.GetMemoryGuardrailKb() > 0) {
        any_guardrail = true;
        break;
      }
    }
    if (!any_guardrail)
      return;

    base::Optional<uint32_t> anon_and_swap;
    std::string status;
    lseek(status_fd_.get(), 0, SEEK_SET);
    if (base::ReadFileDescriptor(*status_fd_, &status))
      anon_and_swap = GetRssAnonAndSwap(status);

    if (!anon_and_swap) {
      PERFETTO_ELOG("Failed to read memory usage.");
      return;
    }

    for (auto it = begin; it != end; ++it) {
      auto& ds = it->second;
      uint32_t ds_max_mem = ds.GetMemoryGuardrailKb();
      if (ds_max_mem > 0 && *anon_and_swap > ds_max_mem) {
        PERFETTO_ELOG("Exceeded data-source memory guardrail (%" PRIu32
                      " > %" PRIu32 "). Shutting down.",
                      *anon_and_swap, ds_max_mem);
        guardrail_hit_callback(&ds);
      }
    }
  }

 private:
  base::ScopedFile status_fd_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_COMMON_PROFILER_GUARDRAILS_H_
