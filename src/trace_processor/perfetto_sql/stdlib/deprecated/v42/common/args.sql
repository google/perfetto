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

-- Returns the formatted value of a given argument.
-- Similar to EXTRACT_ARG, but instead of returning the raw value, it returns
-- the value formatted according to the 'value_type' column (e.g. for booleans,
-- EXTRACT_ARG will return 0 or 1, while FORMATTED_ARG will return 'true' or
-- 'false').
CREATE PERFETTO FUNCTION formatted_arg(
  -- Id of the arg set.
  arg_set_id INT,
  -- Key of the argument.
  arg_key STRING
)
-- Formatted value of the argument.
RETURNS STRING AS
SELECT display_value
FROM args
WHERE arg_set_id = $arg_set_id AND key = $arg_key;