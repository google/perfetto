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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_

namespace perfetto::trace_processor {

// Cross-connection state shared by every PerfettoSqlEngine attached to the
// same TraceProcessorImpl. Owned by TraceProcessorImpl and passed by pointer
// into each connection at construction time.
//
// In the multi-connection design this object holds:
//   - the vtab-state map, populated on writer `OnCommit` and consulted by
//     reader connections during cold xConnect;
//   - the function pool, an additive-only registry diffed against by each
//     connection at the start of `Execute` (no DROP, ever);
//   - per-module include locks that serialise concurrent
//     `INCLUDE PERFETTO MODULE` invocations against the same module name.
//
// This is a Phase 1 skeleton: it has no state and no behaviour. Phase 2
// fills it in. It exists now so that ownership wiring can be reviewed in
// isolation and so subsequent chunks can add private members without
// touching the constructor's call site.
class GlobalStagingArea {
 public:
  GlobalStagingArea();
  ~GlobalStagingArea();

  GlobalStagingArea(const GlobalStagingArea&) = delete;
  GlobalStagingArea& operator=(const GlobalStagingArea&) = delete;

  GlobalStagingArea(GlobalStagingArea&&) = delete;
  GlobalStagingArea& operator=(GlobalStagingArea&&) = delete;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
