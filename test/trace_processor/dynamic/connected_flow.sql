--
-- Copyright 2020 The Android Open Source Project
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
SELECT "connected" as type, s.name, s1.name as start_name, s2.name as end_name  FROM slice s
JOIN CONNECTED_FLOW(s.id) c
JOIN slice s1 ON s1.id = c.slice_out
JOIN slice s2 ON s2.id = c.slice_in
UNION
SELECT "following" as type, s.name, s1.name as start_name, s2.name as end_name  FROM slice s
JOIN FOLLOWING_FLOW(s.id) c
JOIN slice s1 ON s1.id = c.slice_out
JOIN slice s2 ON s2.id = c.slice_in
UNION
SELECT "preceding" as type, s.name, s1.name as start_name, s2.name as end_name  FROM slice s
JOIN PRECEDING_FLOW(s.id) c
JOIN slice s1 ON s1.id = c.slice_out
JOIN slice s2 ON s2.id = c.slice_in
ORDER BY type, s.name, s1.name, s2.name ASC
