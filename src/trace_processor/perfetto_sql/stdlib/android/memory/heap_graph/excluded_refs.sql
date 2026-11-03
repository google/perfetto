--
-- Copyright 2024 The Android Open Source Project
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

CREATE PERFETTO TABLE _ref_type_ids AS
SELECT id AS type_id
FROM heap_graph_class
WHERE
  kind IN (
    'KIND_FINALIZER_REFERENCE',
    'KIND_PHANTOM_REFERENCE',
    'KIND_WEAK_REFERENCE'
  )
ORDER BY
  type_id;

CREATE PERFETTO TABLE _excluded_refs AS
WITH
  ref_objects AS (
    SELECT reference_set_id
    FROM heap_graph_object
    WHERE
      reference_set_id IS NOT NULL
      AND type_id IN _ref_type_ids
  )
SELECT ref.id
FROM ref_objects
CROSS JOIN heap_graph_reference AS ref USING (reference_set_id)
WHERE
  ref.field_name = 'java.lang.ref.Reference.referent'
ORDER BY
  ref.id;

-- Strong reference edges of the heap graph: references out of reachable
-- objects, minus the referent of weak, phantom and finalizer references.
CREATE PERFETTO VIEW _heap_graph_strong_edges(
  -- Object holding the reference.
  src LONG,
  -- Object referred to.
  dst LONG
)
AS
SELECT ref.owner_id AS src, ref.owned_id AS dst
FROM heap_graph_object AS o
CROSS JOIN heap_graph_reference AS ref USING (reference_set_id)
WHERE
  o.reachable = 1
  AND o.reference_set_id IS NOT NULL
  AND ref.owned_id IS NOT NULL
  AND NOT (o.type_id IN _ref_type_ids
  AND ref.field_name = 'java.lang.ref.Reference.referent');
