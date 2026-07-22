// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import type {Trace} from '../public/trace';
import {NUM} from '../trace_processor/query_result';

// Whether the trace has meaningful timeline content. Plugins whose traces are
// pure profiles (heap dumps, pprof archives, ...) use this to decide if a
// profile-specific landing page should be suggested instead of the viewer.
export async function traceHasTimelineData(ctx: Trace): Promise<boolean> {
  // We treat a small number of slices as not having timeline data cos
  // there are some inevitable slices like trace triggers on oom etc.
  const res = await ctx.engine.query(`
    SELECT
      (SELECT count(id) FROM slice) > 50 OR
      EXISTS(SELECT 1 FROM sched) OR
      EXISTS(SELECT 1 FROM heap_profile_allocation) OR
      EXISTS(SELECT 1 FROM perf_sample)
      AS res
  `);
  return res.firstRow({res: NUM}).res > 0;
}
