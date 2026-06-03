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

-- This query returns all objects that are referenced (owned) by objects of class <owner_classname>
-- and are instances of class <owned_classname>
-- This query is useful to find out details about objects dominated by a class
SELECT
  c_owned.name AS owned_class_name,
  o_owned.self_size AS owned_size,
  c_owner.name AS owner_class_name,
  r.owner_id AS owner_id,
  o_owned.id AS owned_id
FROM heap_graph_object AS o_owner
JOIN heap_graph_class AS c_owner
  ON o_owner.type_id = c_owner.id
JOIN heap_graph_reference AS r
  ON r.owner_id = o_owner.id
JOIN heap_graph_object AS o_owned
  ON r.owned_id = o_owned.id
JOIN heap_graph_class AS c_owned
  ON o_owned.type_id = c_owned.id
WHERE
  c_owner.name = '<owner_classname>'
  AND c_owned.name = '<owned_classname>';
