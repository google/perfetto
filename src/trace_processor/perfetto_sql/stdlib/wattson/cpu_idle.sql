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

INCLUDE PERFETTO MODULE android.device;
INCLUDE PERFETTO MODULE cpu.idle;

-- Device specific info for deep idle time offsets
CREATE PERFETTO TABLE _device_cpu_deep_idle_offsets
AS
WITH data(device, cpu, offset_ns) AS (
  VALUES
  ("oriole", 6, 200000),
  ("oriole", 7, 200000),
  ("raven", 6, 200000),
  ("raven", 7, 200000),
  ("eos", 0, 450000),
  ("eos", 1, 450000),
  ("eos", 2, 450000),
  ("eos", 3, 450000)
)
select * from data;

-- Get the corresponding deep idle time offset based on device and CPU.
CREATE PERFETTO FUNCTION _get_deep_idle_offset(cpu INT)
RETURNS INT
AS
SELECT offset_ns
FROM _device_cpu_deep_idle_offsets as offsets, android_device_name as device
WHERE
  offsets.device = device.name AND cpu = $cpu;

-- Adjust duration of active portion to be slightly longer to account for
-- overhead cost of transitioning out of deep idle. This is done because the
-- device is active and consumes power for longer than the logs actually report.
CREATE PERFETTO FUNCTION _adjust_deep_idle(cpu_match INT)
RETURNS TABLE(ts LONG, dur INT, idle INT) AS
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
  offset_ns AS (
    SELECT IFNULL(_get_deep_idle_offset($cpu_match), 0) as offset_ns
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
      -- ts_next is the starting timestamp of the next slice (e.g. end ts of
      -- current slice)
      ts + dur as ts_next,
      idle
    FROM idle_prev, offset_ns
    WHERE cpu = $cpu_match
  )
SELECT
  ts,
  lead(ts, 1, trace_end()) OVER (ORDER by ts) - ts as dur,
  idle
FROM idle_mod
WHERE ts != ts_next;

-- idle_slices_cpux has CPUx specific idle state slices.
CREATE PERFETTO TABLE _idle_slices_cpu0
AS
SELECT idle as idle_0, ts, dur
FROM _adjust_deep_idle(0);

CREATE PERFETTO TABLE _idle_slices_cpu1
AS
SELECT idle as idle_1, ts, dur
FROM _adjust_deep_idle(1);

CREATE PERFETTO TABLE _idle_slices_cpu2
AS
SELECT idle as idle_2, ts, dur
FROM _adjust_deep_idle(2);

CREATE PERFETTO TABLE _idle_slices_cpu3
AS
SELECT idle as idle_3, ts, dur
FROM _adjust_deep_idle(3);

CREATE PERFETTO TABLE _idle_slices_cpu4
AS
SELECT idle as idle_4, ts, dur
FROM _adjust_deep_idle(4);

CREATE PERFETTO TABLE _idle_slices_cpu5
AS
SELECT idle as idle_5, ts, dur
FROM _adjust_deep_idle(5);

CREATE PERFETTO TABLE _idle_slices_cpu6
AS
SELECT idle as idle_6, ts, dur
FROM _adjust_deep_idle(6);

CREATE PERFETTO TABLE _idle_slices_cpu7
AS
SELECT idle as idle_7, ts, dur
FROM _adjust_deep_idle(7);

-- SPAN_OUTER_JOIN of all CPUs' idle state tables.
CREATE VIRTUAL TABLE _idle_slices_cpu01
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu0, _idle_slices_cpu1);

CREATE VIRTUAL TABLE _idle_slices_cpu012
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu01, _idle_slices_cpu2);

CREATE VIRTUAL TABLE _idle_slices_cpu0123
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu012, _idle_slices_cpu3);

CREATE VIRTUAL TABLE _idle_slices_cpu01234
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu0123, _idle_slices_cpu4);

CREATE VIRTUAL TABLE _idle_slices_cpu012345
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu01234, _idle_slices_cpu5);

CREATE VIRTUAL TABLE _idle_slices_cpu0123456
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu012345, _idle_slices_cpu6);

CREATE VIRTUAL TABLE _idle_slices_cpu01234567
USING
  SPAN_OUTER_JOIN(_idle_slices_cpu0123456, _idle_slices_cpu7);

-- Table that holds time slices of the entire trace with the idle state
-- transition information of every CPU in the system.
CREATE PERFETTO TABLE _cpu_idle_all
AS
SELECT * FROM _idle_slices_cpu01234567;

