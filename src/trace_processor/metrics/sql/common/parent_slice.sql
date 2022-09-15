--
-- Copyright 2022 The Android Open Source Project
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
--

SELECT CREATE_FUNCTION(
  'HAS_PARENT_SLICE_WITH_NAME(id INT, parent_name STRING)',
  'BOOL',
  '
    SELECT EXISTS(
      SELECT 1
      FROM ancestor_slice($id)
      WHERE name = $parent_name
      LIMIT 1
    );
  '
);
