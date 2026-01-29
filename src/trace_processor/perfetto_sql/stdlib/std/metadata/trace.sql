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

-- @module std.metadata.trace
-- Trace-level metadata functions and tables.

-- Returns a comma-separated list of machine names associated with a trace.
CREATE PERFETTO FUNCTION metadata_get_trace_machines(
    -- Trace id.
    trace_id LONG
)
-- Returns a string containing the comma-separated machine names.
RETURNS STRING AS
SELECT
  GROUP_CONCAT(
    coalesce(metadata_get_machine_str(machine_id, 'system_name'), 'Machine ' || machine_id),
    ', '
  )
FROM (
  SELECT DISTINCT
    machine_id
  FROM metadata
  WHERE
    trace_id = $trace_id AND machine_id IS NOT NULL
);

-- A table containing pivoted metadata for each trace in the session.
CREATE PERFETTO TABLE metadata_by_trace (
  -- Trace identifier.
  trace_id LONG,
  -- Unique session name.
  unique_session_name STRING,
  -- Trace UUID.
  trace_uuid STRING,
  -- Trace type.
  trace_type STRING,
  -- Trace size in bytes.
  trace_size_bytes LONG,
  -- Trace trigger.
  trace_trigger STRING,
  -- Comma-separated list of machine names associated with this trace.
  machines STRING
) AS
SELECT
  trace_id,
  metadata_get_trace_str(trace_id, 'unique_session_name') AS unique_session_name,
  metadata_get_trace_str(trace_id, 'trace_uuid') AS trace_uuid,
  metadata_get_trace_str(trace_id, 'trace_type') AS trace_type,
  metadata_get_trace_int(trace_id, 'trace_size_bytes') AS trace_size_bytes,
  metadata_get_trace_str(trace_id, 'trace_trigger') AS trace_trigger,
  metadata_get_trace_machines(trace_id) AS machines
FROM (
  SELECT DISTINCT
    trace_id
  FROM metadata
  WHERE
    trace_id IS NOT NULL
);
