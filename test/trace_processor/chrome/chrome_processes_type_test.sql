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
SELECT pid, name, string_value AS chrome_process_type
FROM
    process
-- EXTRACT_ARG doesn't make sense here - it would get chrome.process_type
-- (or NULL) for each process in the trace. But what we want is - a subset
-- of process that have a "chrome.process_type" argument, whether it's NULL or
-- not.
JOIN
    (SELECT * FROM args WHERE key = "chrome.process_type") chrome_process_args
ON
    process.arg_set_id = chrome_process_args.arg_set_id
ORDER BY pid;
