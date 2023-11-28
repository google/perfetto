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

SELECT
  EXTRACT_ARG(s.arg_set_id, 'track_event.log_message') AS log_message,
  EXTRACT_ARG(s.arg_set_id, 'track_event.log_message.function_name') AS function_name,
  EXTRACT_ARG(s.arg_set_id, 'track_event.log_message.file_name') AS file_name,
  EXTRACT_ARG(s.arg_set_id, 'track_event.log_message.line_number') AS line_number
FROM
  slice s;
