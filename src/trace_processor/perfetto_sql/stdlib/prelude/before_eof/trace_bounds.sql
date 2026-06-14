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

-- The values are being filled by Trace Processor when parsing the trace.
-- Exposed with `trace_bounds`. Kept as a plain table: it is populated by the
-- engine at parse time, not derived from a pipeline.
CREATE TABLE _trace_bounds AS
SELECT 0 AS start_ts, 0 AS end_ts;

-- Fetch start of the trace.
CREATE PERFETTO FUNCTION trace_start()
-- Start of the trace.
RETURNS TIMESTAMP
AS
FROM _trace_bounds
|> SELECT start_ts;

-- Fetch end of the trace.
CREATE PERFETTO FUNCTION trace_end()
-- End of the trace.
RETURNS TIMESTAMP
AS
FROM _trace_bounds
|> SELECT end_ts;

-- Fetch duration of the trace.
CREATE PERFETTO FUNCTION trace_dur()
-- Duration of the trace.
RETURNS DURATION
AS
SELECT trace_end() - trace_start();
