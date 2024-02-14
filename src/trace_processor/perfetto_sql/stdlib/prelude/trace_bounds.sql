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

-- Definition of `trace_bounds` table. The values are being filled by Trace
-- Processor when parsing the trace. Can't be a Perfetto table because it has
-- to be mutable. 
-- It is recommended to depend on the `trace_start()` and `trace_end()`
-- functions rather than directly on `trace_bounds`.
CREATE TABLE trace_bounds AS
SELECT 0 AS start_ts, 0 AS end_ts;

-- Fetch start of the trace.
CREATE PERFETTO FUNCTION trace_start()
-- Start of the trace in nanoseconds.
RETURNS LONG AS
SELECT start_ts FROM trace_bounds;

-- Fetch end of the trace.
CREATE PERFETTO FUNCTION trace_end()
-- End of the trace in nanoseconds.
RETURNS LONG AS
SELECT end_ts FROM trace_bounds;

-- Fetch duration of the trace.
CREATE PERFETTO FUNCTION trace_dur()
-- Duration of the trace in nanoseconds.
RETURNS LONG AS
SELECT trace_end() - trace_start();