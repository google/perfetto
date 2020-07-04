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
select perf_sample.ts, thread.tid, perf_sample.cpu, perf_sample.cpu_mode,
       perf_sample.unwind_error, concatenated_callsite.frames_str
from perf_sample
join thread
  using(utid)
left join (
  select flattened_callsite.id, group_concat(spf.name) frames_str
  from (
    with recursive rec(id, parent_id, frame_id, depth)
      as (
        select id, parent_id, frame_id, depth
        from stack_profile_callsite
        union all
          select rec.id, spc.parent_id, spc.frame_id, spc.depth
          from stack_profile_callsite spc
          join rec
            on spc.id == rec.parent_id
      )
    select id, frame_id, depth
    from rec
    order by id asc, depth desc
  ) as flattened_callsite
  left join stack_profile_frame spf
    on flattened_callsite.frame_id == spf.id
  group by flattened_callsite.id
) as concatenated_callsite
  on perf_sample.callsite_id == concatenated_callsite.id;
