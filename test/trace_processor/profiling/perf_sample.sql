--
-- Copyright 2020 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

select ps.ts, ps.cpu, ps.cpu_mode, ps.unwind_error, ps.perf_session_id,
       pct.name cntr_name, pct.is_timebase,
       thread.tid,
       spf.name
from experimental_annotated_callstack eac
join perf_sample ps
  on (eac.start_id == ps.callsite_id)
join perf_counter_track pct
  using(perf_session_id, cpu)
join thread
  using(utid)
join stack_profile_frame spf
  on (eac.frame_id == spf.id)
order by ps.ts asc, eac.depth asc

