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

-- Per heap graph class: display name and type classification.
CREATE PERFETTO TABLE _heap_graph_class_classification(
  -- Class type id (heap_graph_class.id).
  id LONG,
  -- Class name (deobfuscated if available).
  type_name STRING,
  -- Whether the class is part of standard Java/Kotlin runtime libraries.
  is_language_runtime BOOL,
  -- Whether the class is an array or standard collection/container.
  is_container BOOL,
  -- Whether the object is a java.lang.Class<*> singleton.
  is_static_class BOOL
)
AS
WITH
  normalized AS MATERIALIZED (
    SELECT
      id,
      coalesce(deobfuscated_name, name) AS type_name,
      replace(
        coalesce(
          rtrim(
            str_split(coalesce(deobfuscated_name, name), 'java.lang.Class<', 1),
            '>'
          ),
          coalesce(deobfuscated_name, name)
        ),
        'j$.',
        'java.'
      ) AS normalized_name
    FROM heap_graph_class
  )
SELECT
  id,
  type_name,
  (normalized_name GLOB 'java.*'
  OR normalized_name GLOB 'kotlin.*'
  OR normalized_name GLOB 'kotlinx.*'
  OR normalized_name GLOB 'sun.*'
  OR normalized_name GLOB 'dalvik.*'
  OR normalized_name GLOB 'libcore.*') AS is_language_runtime,
  (normalized_name GLOB '*[[]]'
  OR normalized_name GLOB 'android.util.*Array*'
  OR normalized_name GLOB 'kotlin.collections.*'
  OR normalized_name GLOB 'androidx.collection.*'
  OR CASE str_split(normalized_name, '$', 0)
    -- java.util concrete collection and map classes
    WHEN 'java.util.ArrayDeque' THEN 1
    WHEN 'java.util.ArrayList' THEN 1
    WHEN 'java.util.Arrays' THEN 1
    WHEN 'java.util.BitSet' THEN 1
    WHEN 'java.util.Collections' THEN 1
    WHEN 'java.util.EnumMap' THEN 1
    WHEN 'java.util.HashMap' THEN 1
    WHEN 'java.util.HashSet' THEN 1
    WHEN 'java.util.Hashtable' THEN 1
    WHEN 'java.util.IdentityHashMap' THEN 1
    WHEN 'java.util.ImmutableCollections' THEN 1
    WHEN 'java.util.LinkedHashMap' THEN 1
    WHEN 'java.util.LinkedHashSet' THEN 1
    WHEN 'java.util.LinkedList' THEN 1
    WHEN 'java.util.PriorityQueue' THEN 1
    WHEN 'java.util.Properties' THEN 1
    WHEN 'java.util.RegularEnumSet' THEN 1
    WHEN 'java.util.Stack' THEN 1
    WHEN 'java.util.TreeMap' THEN 1
    WHEN 'java.util.TreeSet' THEN 1
    WHEN 'java.util.Vector' THEN 1
    WHEN 'java.util.WeakHashMap' THEN 1
    -- java.util.concurrent concrete collection and map classes
    WHEN 'java.util.concurrent.ArrayBlockingQueue' THEN 1
    WHEN 'java.util.concurrent.ConcurrentHashMap' THEN 1
    WHEN 'java.util.concurrent.ConcurrentLinkedDeque' THEN 1
    WHEN 'java.util.concurrent.ConcurrentLinkedQueue' THEN 1
    WHEN 'java.util.concurrent.ConcurrentSkipListMap' THEN 1
    WHEN 'java.util.concurrent.ConcurrentSkipListSet' THEN 1
    WHEN 'java.util.concurrent.CopyOnWriteArrayList' THEN 1
    WHEN 'java.util.concurrent.CopyOnWriteArraySet' THEN 1
    WHEN 'java.util.concurrent.DelayQueue' THEN 1
    WHEN 'java.util.concurrent.LinkedBlockingDeque' THEN 1
    WHEN 'java.util.concurrent.LinkedBlockingQueue' THEN 1
    WHEN 'java.util.concurrent.LinkedTransferQueue' THEN 1
    WHEN 'java.util.concurrent.PriorityBlockingQueue' THEN 1
    WHEN 'java.util.concurrent.SynchronousQueue' THEN 1
    ELSE 0
  END
  = 1) AS is_container,
  type_name GLOB 'java.lang.Class<*>' AS is_static_class
FROM normalized
ORDER BY
  id;
