--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- sqlformat file off

-- Replaces all occurrences of a regular expression with a constant 
-- replacement string.
-- Note that there is no way to substitute matching groups into the 
-- replacement, and all matching is case-sensitive.
CREATE PERFETTO FUNCTION regexp_replace_simple(
    -- The input string to match against.
    input STRING,
    -- The matching regexp.
    regex STRING,
    -- The replacement string to substitute in.
    replacement STRING
)
-- The result.
RETURNS STRING DELEGATES TO __intrinsic_regexp_replace_simple;

-- sqlformat file on
