--
-- Copyright 2022 The Android Open Source Project
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

SELECT content.total_size,
  frame.field_type, frame.field_name,
  frame.parent_id,
  EXTRACT_ARG(frame.arg_set_id, 'event.category') AS event_category,
  EXTRACT_ARG(frame.arg_set_id, 'event.name') AS event_name
FROM experimental_proto_path AS frame JOIN experimental_proto_content AS content ON content.path_id = frame.id
ORDER BY total_size DESC, path
LIMIT 10;
