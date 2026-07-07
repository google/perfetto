--
-- Copyright 2026 The Android Open Source Project
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

-- Diagnostics detecting poorly-written trace configs (and other trace-quality
-- problems), one row per detected issue. Written by TraceDiagnosticsTracker.
CREATE PERFETTO VIEW trace_diagnostics(
  -- Unique identifier for this diagnostic row.
  id ID,
  -- Stable diagnostic key (e.g. tiny_ftrace_buffer).
  name STRING,
  -- Human-readable explanation of the problem.
  description STRING,
  -- Suggested fix for the problem.
  remediation STRING,
  -- Detector confidence in [0.0, 1.0] that this is a real problem.
  confidence DOUBLE,
  -- Machine this diagnostic applies to.
  machine_id JOINID(machine.id),
  -- Trace this diagnostic applies to.
  trace_id LONG
)
AS
SELECT id, name, description, remediation, confidence, machine_id, trace_id
FROM __intrinsic_trace_diagnostics;
