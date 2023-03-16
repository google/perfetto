--
-- Copyright 2023 The Android Open Source Project
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

-- Events when CPU entered hypervisor.
--
-- @column slice_id       Id of the corresponding slice in slices table.
-- @column cpu            CPU that entered hypervisor.
-- @column ts             Timestamp when CPU entered hypervisor (in nanoseconds).
-- @column dur            How much time CPU spent in hypervisor (in nanoseconds).
-- @column reason         Reason for entering hypervisor (e.g. host_hcall, host_mem_abort), or NULL if unknown.
CREATE VIEW pkvm_hypervisor_events AS
SELECT
  slices.id as slice_id,
  cpu_track.cpu as cpu,
  slices.ts as ts,
  slices.dur as dur,
  EXTRACT_ARG(slices.arg_set_id, 'hyp_enter_reason') as reason
FROM slices
JOIN cpu_track ON cpu_track.id = slices.track_id
WHERE
  slices.category = 'pkvm_hyp'
