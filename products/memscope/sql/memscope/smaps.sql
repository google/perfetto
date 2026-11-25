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

-- Classifies an smaps mapping path into a Memscope memory category: 'java',
-- 'native', 'stack', 'graphics', 'file' or 'other'.
CREATE PERFETTO FUNCTION memscope_smaps_category(
  -- Path of the mapping, as in profiler_smaps.path.
  path STRING
)
-- The category.
RETURNS STRING
AS
SELECT
  CASE
    WHEN $path GLOB '*dalvik*'
    OR $path GLOB '/dev/ashmem/dalvik*' THEN 'java'
    WHEN $path GLOB '[anon:scudo*'
    OR $path GLOB '[anon:libc_malloc*'
    OR $path GLOB '[anon:jemalloc*'
    OR $path GLOB '[anon:GWP-ASan*'
    OR $path = '[heap]' THEN 'native'
    WHEN $path = '[stack]'
    OR $path GLOB '[anon:stack*' THEN 'stack'
    WHEN $path GLOB '/dev/kgsl*'
    OR $path GLOB '/dev/mali*'
    OR $path GLOB '/dev/dri*'
    OR $path GLOB '*dmabuf*' THEN 'graphics'
    WHEN $path GLOB '/*' THEN 'file'
    ELSE 'other'
  END;

-- Classifies a file-backed smaps mapping path into a bucket: 'so',
-- 'java_code', 'resources' or 'other_file'.
CREATE PERFETTO FUNCTION memscope_smaps_file_bucket(
  -- Path of the mapping, as in profiler_smaps.path.
  path STRING
)
-- The bucket.
RETURNS STRING
AS
SELECT
  CASE
    WHEN $path GLOB '*.so'
    OR $path GLOB '*.so.*' THEN 'so'
    WHEN $path GLOB '*.jar'
    OR $path GLOB '*.oat'
    OR $path GLOB '*.odex'
    OR $path GLOB '*.vdex'
    OR $path GLOB '*.art'
    OR $path GLOB '*.dex' THEN 'java_code'
    WHEN $path GLOB '*.apk'
    OR $path GLOB '*.ttf'
    OR $path GLOB '*.otf'
    OR $path GLOB '*.dat' THEN 'resources'
    ELSE 'other_file'
  END;

-- Per smaps snapshot and memory category, the bytes of one process.
CREATE PERFETTO TABLE memscope_smaps_category_breakdown(
  -- Process id.
  upid JOINID(process.id),
  -- Timestamp of the smaps snapshot.
  ts TIMESTAMP,
  -- Category, see memscope_smaps_category.
  category STRING,
  -- Resident bytes.
  rss_bytes LONG,
  -- Swapped bytes.
  swap_bytes LONG,
  -- Anonymous bytes.
  anon_bytes LONG,
  -- Resident + swapped bytes: what the composition chart stacks.
  rss_swap_bytes LONG,
  -- Anonymous + swapped bytes.
  anon_swap_bytes LONG
)
AS
SELECT
  upid,
  ts,
  memscope_smaps_category(path) AS category,
  cast_int!(ifnull(sum(rss_kb), 0) * 1024) AS rss_bytes,
  cast_int!(ifnull(sum(swap_kb), 0) * 1024) AS swap_bytes,
  cast_int!(ifnull(sum(anonymous_kb), 0) * 1024) AS anon_bytes,
  cast_int!(ifnull(sum(rss_kb + swap_kb), 0) * 1024) AS rss_swap_bytes,
  cast_int!(ifnull(sum(anonymous_kb + swap_kb), 0) * 1024) AS anon_swap_bytes
FROM profiler_smaps
GROUP BY
  upid,
  ts,
  category
ORDER BY
  upid,
  ts,
  category;

-- Per smaps snapshot, the totals of one process.
CREATE PERFETTO TABLE memscope_smaps_snapshot(
  -- Process id.
  upid JOINID(process.id),
  -- Timestamp of the smaps snapshot.
  ts TIMESTAMP,
  -- Resident + swapped bytes over all mappings.
  rss_swap_bytes LONG,
  -- Anonymous + swapped bytes over all mappings. The "RSS" Memscope reports.
  anon_swap_bytes LONG
)
AS
SELECT
  upid,
  ts,
  cast_int!(ifnull(sum(rss_kb + swap_kb), 0) * 1024) AS rss_swap_bytes,
  cast_int!(ifnull(sum(anonymous_kb + swap_kb), 0) * 1024) AS anon_swap_bytes
FROM profiler_smaps
GROUP BY
  upid,
  ts
ORDER BY
  upid,
  ts;

-- Per smaps snapshot and file bucket, the resident + swapped bytes of the
-- file-backed mappings of one process.
CREATE PERFETTO TABLE memscope_smaps_file_breakdown(
  -- Process id.
  upid JOINID(process.id),
  -- Timestamp of the smaps snapshot.
  ts TIMESTAMP,
  -- Bucket, see memscope_smaps_file_bucket.
  bucket STRING,
  -- Resident + swapped bytes.
  rss_swap_bytes LONG
)
AS
SELECT
  upid,
  ts,
  memscope_smaps_file_bucket(path) AS bucket,
  cast_int!(ifnull(sum(rss_kb + swap_kb), 0) * 1024) AS rss_swap_bytes
FROM profiler_smaps
WHERE
  path GLOB '/*'
GROUP BY
  upid,
  ts,
  bucket
ORDER BY
  upid,
  ts,
  bucket;
