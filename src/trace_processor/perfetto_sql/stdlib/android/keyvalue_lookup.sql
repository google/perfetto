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

-- Contains utility functions for extracting key-value pairs from ATrace payloads.

-- Extracts the value associated with a given key from a string containing key-value pairs.
-- The payload string is expected to be in a format like '{ key1=value1 key2="value2" ... }'.
-- This function handles both quoted and unquoted values.
CREATE PERFETTO FUNCTION _android_keyvalue_lookup_extract_key_value_arg(
  -- The string containing key-value pairs, typically from an ATrace event payload.
  atrace_payload STRING,
  -- The name of the key whose value is to be extracted.
  key_name STRING
)
-- Returns the extracted value as a STRING, or NULL if the key is not found.
RETURNS STRING
AS
WITH
  prep AS (
    SELECT
      substr(
        $atrace_payload,
        instr($atrace_payload, $key_name || '=') + length($key_name || '=')
      ) AS payload_after_key
    FROM (SELECT $atrace_payload, $key_name)
    WHERE
      $atrace_payload GLOB '*' || $key_name || '=*'
  ),
  extracted AS (
    SELECT
      payload_after_key,
      CASE
        -- Value starts with a quote
        WHEN substr(payload_after_key, 1, 1) = '"' THEN instr(
          substr(payload_after_key, 2),
          '"'
        )
        ELSE (
          SELECT min(pos)
          FROM (
            SELECT instr(payload_after_key || ' ', ' ') AS pos
            UNION ALL
            SELECT instr(payload_after_key || '}', '}') AS pos
          )
          WHERE
            pos > 0
        )
        - 1
      END AS end_idx
    FROM prep
  )
SELECT trim(replace(substr(payload_after_key, 1, end_idx), '"', ''))
FROM extracted;
