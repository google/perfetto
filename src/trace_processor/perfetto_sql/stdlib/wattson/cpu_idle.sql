--
-- Copyright 2024 The Android Open Source Project
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

INCLUDE PERFETTO MODULE linux.cpu.idle;
INCLUDE PERFETTO MODULE wattson.device_infos;

-- Get the corresponding deep idle time offset based on device and CPU.
CREATE PERFETTO TABLE _filtered_deep_idle_offsets
AS
SELECT cpu, offset_ns
FROM _device_cpu_deep_idle_offsets as offsets
JOIN _wattson_device as device
ON offsets.device = device.name;

-- Adjust duration of active portion to be slightly longer to account for
-- overhead cost of transitioning out of deep idle. This is done because the
-- device is active and consumes power for longer than the logs actually report.
CREATE PERFETTO TABLE _adjusted_deep_idle
AS
WITH
  idle_prev AS (
    SELECT
      ts,
      dur,
      idle,
      lag(idle) OVER (PARTITION BY track_id ORDER BY ts) AS idle_prev,
      cpu
    FROM cpu_idle_counters
  ),
  -- Adjusted ts if applicable, which makes the current deep idle state
  -- slightly shorter.
  idle_mod AS (
    SELECT
      IIF(
        idle_prev = -1 AND idle = 1,
        IIF(dur > offset_ns, ts + offset_ns, ts + dur),
        ts
      ) as ts,
      -- ts_next is the starting timestamp of the next slice (i.e. end ts of
      -- current slice)
      ts + dur as ts_next,
      cpu,
      idle
    FROM idle_prev
    JOIN _filtered_deep_idle_offsets using (cpu)
  )
SELECT
  ts,
  lead(ts, 1, trace_end()) OVER (PARTITION BY cpu ORDER by ts) - ts as dur,
  cpu,
  idle
FROM idle_mod
WHERE ts != ts_next;

