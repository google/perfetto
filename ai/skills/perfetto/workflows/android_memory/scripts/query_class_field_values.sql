-- Copyright (C) 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Extracts String and primitive field value distributions and per-instance
-- field configuration tuples for reachable Java heap objects.
INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

CREATE OR REPLACE PERFETTO TABLE _diff_all_obj_sizes AS
SELECT
  o.id,
  o.object_data_id,
  o.upid,
  o.graph_sample_ts,
  o.heap_type,
  COALESCE(c.deobfuscated_name, c.name) AS class_name,
  o.self_size,
  COALESCE(dt.dominated_size_bytes, o.self_size) AS dominated_size
FROM heap_graph_object AS o
JOIN heap_graph_class AS c ON o.type_id = c.id
LEFT JOIN heap_graph_dominator_tree AS dt ON o.id = dt.id
WHERE
  o.reachable = 1
  AND COALESCE(c.deobfuscated_name, c.name) NOT GLOB 'java.*'
  AND COALESCE(c.deobfuscated_name, c.name) NOT GLOB 'sun.*'
  AND COALESCE(c.deobfuscated_name, c.name) NOT GLOB 'dalvik.*'
  AND COALESCE(c.deobfuscated_name, c.name) NOT GLOB 'libcore.*'
  AND COALESCE(c.deobfuscated_name, c.name) NOT GLOB 'android.icu.*'
  AND COALESCE(c.deobfuscated_name, c.name) NOT GLOB '*[]';

CREATE OR REPLACE PERFETTO TABLE _diff_top_classes AS
SELECT class_name
FROM _diff_all_obj_sizes
GROUP BY
  class_name
ORDER BY
  SUM(dominated_size) DESC
LIMIT 25;

CREATE OR REPLACE PERFETTO TABLE _diff_obj_sizes AS
SELECT o.*
FROM _diff_all_obj_sizes AS o
JOIN _diff_top_classes AS tc USING (class_name);

CREATE OR REPLACE PERFETTO TABLE _diff_obj_field_entries AS
SELECT
  o.id AS obj_id,
  o.upid,
  o.graph_sample_ts,
  o.heap_type,
  o.class_name,
  o.self_size,
  o.dominated_size,
  r.field_name,
  r.field_type_name AS field_type,
  ''''
  || REPLACE(
    REPLACE(
      REPLACE(COALESCE(owned_od.value_string, '[empty]'), CHAR(10), '\n'),
      CHAR(13),
      '\r'
    ),
    '|',
    '\|'
  )
  || '''' AS field_value
FROM _diff_obj_sizes AS o
JOIN heap_graph_reference AS r ON o.id = r.owner_id
JOIN heap_graph_object AS owned_obj ON r.owned_id = owned_obj.id
JOIN heap_graph_class AS owned_cls ON owned_obj.type_id = owned_cls.id
LEFT JOIN heap_graph_object_data AS owned_od
  ON owned_obj.object_data_id = owned_od.id
WHERE
  COALESCE(owned_cls.deobfuscated_name, owned_cls.name) = 'java.lang.String'
  AND r.field_name IS NOT NULL
  AND owned_od.value_string IS NOT NULL
UNION ALL
SELECT
  o.id AS obj_id,
  o.upid,
  o.graph_sample_ts,
  o.heap_type,
  o.class_name,
  o.self_size,
  o.dominated_size,
  p.field_name,
  p.field_type,
  CASE
    WHEN p.field_type = 'boolean' THEN CASE
      WHEN p.bool_value = 1 THEN 'true'
      ELSE 'false'
    END
    WHEN p.field_type = 'byte' THEN CAST(p.byte_value AS TEXT)
    WHEN p.field_type = 'char' THEN CAST(p.char_value AS TEXT)
    WHEN p.field_type = 'short' THEN CAST(p.short_value AS TEXT)
    WHEN p.field_type = 'int' THEN CAST(p.int_value AS TEXT)
    WHEN p.field_type = 'long' THEN CAST(p.long_value AS TEXT)
    WHEN p.field_type = 'float' THEN CAST(p.float_value AS TEXT)
    WHEN p.field_type = 'double' THEN CAST(p.double_value AS TEXT)
    ELSE 'null'
  END AS field_value
FROM _diff_obj_sizes AS o
JOIN heap_graph_object_data AS od ON o.object_data_id = od.id
JOIN heap_graph_primitive AS p ON od.field_set_id = p.field_set_id
WHERE
  p.field_name IS NOT NULL
  AND p.field_name NOT LIKE '%shadow$%'
  AND p.field_name NOT LIKE '%.mObject'
  AND p.field_name NOT LIKE '%mNativePtr%'
  AND p.field_name NOT LIKE '%mNativeObj%'
  AND p.field_name NOT LIKE '%mNativeContext%'
  AND p.field_name NOT LIKE '%mNativeInstance%';

CREATE OR REPLACE PERFETTO TABLE _diff_obj_config_tuples AS
WITH
  ordered_fields AS (
    SELECT
      obj_id,
      upid,
      graph_sample_ts,
      heap_type,
      class_name,
      self_size,
      dominated_size,
      field_name,
      field_value
    FROM _diff_obj_field_entries
    ORDER BY
      obj_id,
      field_name
  )
SELECT
  obj_id,
  upid,
  graph_sample_ts,
  heap_type,
  class_name,
  self_size,
  dominated_size,
  GROUP_CONCAT(field_name || '=' || field_value, ', ') AS config_tuple
FROM ordered_fields
GROUP BY
  obj_id,
  upid,
  graph_sample_ts,
  heap_type,
  class_name,
  self_size,
  dominated_size;

SELECT
  'field_value' AS section,
  COALESCE(pr.name, 'pid=' || pr.pid) AS process_name,
  f.upid,
  f.graph_sample_ts,
  f.heap_type,
  f.class_name,
  f.field_name,
  f.field_type,
  f.field_value,
  '' AS config_tuple,
  COUNT(*) AS instance_count,
  SUM(f.self_size) AS self_size,
  SUM(f.dominated_size) AS dominated_size
FROM _diff_obj_field_entries AS f
JOIN process AS pr ON f.upid = pr.id
GROUP BY
  f.upid,
  f.graph_sample_ts,
  f.heap_type,
  f.class_name,
  f.field_name,
  f.field_type,
  f.field_value
UNION ALL
SELECT
  'instance_config' AS section,
  COALESCE(pr.name, 'pid=' || pr.pid) AS process_name,
  c.upid,
  c.graph_sample_ts,
  c.heap_type,
  c.class_name,
  '' AS field_name,
  '' AS field_type,
  '' AS field_value,
  c.config_tuple,
  COUNT(*) AS instance_count,
  SUM(c.self_size) AS self_size,
  SUM(c.dominated_size) AS dominated_size
FROM _diff_obj_config_tuples AS c
JOIN process AS pr ON c.upid = pr.id
GROUP BY
  c.upid,
  c.graph_sample_ts,
  c.heap_type,
  c.class_name,
  c.config_tuple ORDER BY section, dominated_size DESC, instance_count DESC;
