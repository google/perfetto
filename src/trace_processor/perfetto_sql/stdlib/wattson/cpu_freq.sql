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

INCLUDE PERFETTO MODULE cpu.freq;

-- Filters for CPU specific frequency slices
CREATE PERFETTO FUNCTION _per_cpu_freq_slice(cpu_match INT)
RETURNS TABLE(ts LONG, dur INT, freq INT)
AS
SELECT ts, dur, freq
FROM cpu_freq_counters WHERE cpu = $cpu_match;

-- _freq_slices_cpux has CPUx specific frequency slices.
CREATE PERFETTO TABLE _freq_slices_cpu0
AS
SELECT ts, dur, freq AS freq_0 FROM _per_cpu_freq_slice(0);

CREATE PERFETTO TABLE _freq_slices_cpu1
AS
SELECT ts, dur, freq AS freq_1 FROM _per_cpu_freq_slice(1);

CREATE PERFETTO TABLE _freq_slices_cpu2
AS
SELECT ts, dur, freq AS freq_2 FROM _per_cpu_freq_slice(2);

CREATE PERFETTO TABLE _freq_slices_cpu3
AS
SELECT ts, dur, freq AS freq_3 FROM _per_cpu_freq_slice(3);

CREATE PERFETTO TABLE _freq_slices_cpu4
AS
SELECT ts, dur, freq AS freq_4 FROM _per_cpu_freq_slice(4);

CREATE PERFETTO TABLE _freq_slices_cpu5
AS
SELECT ts, dur, freq AS freq_5 FROM _per_cpu_freq_slice(5);

CREATE PERFETTO TABLE _freq_slices_cpu6
AS
SELECT ts, dur, freq AS freq_6 FROM _per_cpu_freq_slice(6);

CREATE PERFETTO TABLE _freq_slices_cpu7
AS
SELECT ts, dur, freq AS freq_7 FROM _per_cpu_freq_slice(7);

-- SPAN_OUTER_JOIN of all CPUs' frequency tables.
CREATE VIRTUAL TABLE _freq_slices_cpu01
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu0, _freq_slices_cpu1);

CREATE VIRTUAL TABLE _freq_slices_cpu012
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu01, _freq_slices_cpu2);

CREATE VIRTUAL TABLE _freq_slices_cpu0123
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu012, _freq_slices_cpu3);

CREATE VIRTUAL TABLE _freq_slices_cpu01234
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu0123, _freq_slices_cpu4);

CREATE VIRTUAL TABLE _freq_slices_cpu012345
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu01234, _freq_slices_cpu5);

CREATE VIRTUAL TABLE _freq_slices_cpu0123456
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu012345, _freq_slices_cpu6);

CREATE VIRTUAL TABLE _freq_slices_cpu01234567
USING
  SPAN_OUTER_JOIN(_freq_slices_cpu0123456, _freq_slices_cpu7);

-- Table that holds time slices of the trace with the frequency transition
-- information of every CPU in the system.
CREATE PERFETTO TABLE _cpu_freq_all
AS
SELECT * FROM _freq_slices_cpu01234567;

