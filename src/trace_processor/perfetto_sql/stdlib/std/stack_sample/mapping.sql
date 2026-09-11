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

-- Best-effort code origin from a mapping path. Mapping metadata does not
-- distinguish executable ELF files from shared objects, so conventional library
-- suffixes identify libraries; other ordinary filenames are treated as binaries.
-- Symbol availability and process display names do not affect this classification.
CREATE PERFETTO FUNCTION _stack_sample_mapping_category(
  -- Mapping path, including any " (deleted)" suffix.
  mapping_name STRING
)
-- 0: binary, 1: library, 2: kernel, 3: unknown or anonymous.
RETURNS LONG
AS
WITH
  normalized AS (
    SELECT
      replace(
        CASE
          WHEN $mapping_name GLOB '* (deleted)' THEN substr(
            $mapping_name,
            1,
            length($mapping_name) - 10
          )
          ELSE $mapping_name
        END,
        char(92),
        '/'
      ) AS path
  ),
  names AS (
    SELECT
      path,
      str_split(path, '/', length(path) - length(replace(path, '/', ''))) AS name
    FROM normalized
  )
SELECT
  CASE
    WHEN path IS NULL
    OR path = '' THEN 3
    WHEN path GLOB '[[]kernel*'
    OR path = '/kernel'
    OR path GLOB '/kernel/*'
    OR name GLOB 'vmlinux*'
    OR name GLOB 'vmlinuz*'
    OR name GLOB '*.ko'
    OR name GLOB '*.ko.xz'
    OR name GLOB '*.ko.gz'
    OR name GLOB '*.ko.zst' THEN 2
    WHEN name = ''
    OR path GLOB '[[]*'
    OR path IN ('unknown', '[unknown]')
    OR path GLOB 'linux-vdso*'
    OR path GLOB 'memfd:*'
    OR path GLOB '/memfd:*'
    OR path GLOB 'anon_inode:*' THEN 3
    WHEN lower(name) GLOB '*.so'
    OR lower(name) GLOB '*.so.*'
    OR lower(name) GLOB '*.dylib'
    OR lower(name) GLOB '*.dll'
    OR lower(name) GLOB '*.dex'
    OR lower(name) GLOB '*.odex'
    OR lower(name) GLOB '*.oat'
    OR lower(name) GLOB '*.art'
    OR lower(name) GLOB '*.jar'
    OR lower(name) GLOB '*.apk' THEN 1
    ELSE 0
  END
FROM names;

-- Classify once per mapping, shared by sample instants and callstack frames.
CREATE PERFETTO TABLE _stack_sample_mapping_classification AS
SELECT id, name, _stack_sample_mapping_category(name) AS category
FROM stack_profile_mapping;
