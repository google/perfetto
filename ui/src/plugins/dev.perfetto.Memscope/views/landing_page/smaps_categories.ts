// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Shared smaps category taxonomy: the SQL path → category classifier and the
// display metadata (labels / colors) for each category. Used by the
// composition-over-time chart and the memory map.

// Path → category classification. Keys match SMAPS_CATEGORIES. Expects the
// profiler_smaps row to be aliased `s` in the query.
export const SMAPS_CATEGORY_CASE_SQL = `
  CASE
    WHEN s.path GLOB '*dalvik*' OR s.path GLOB '/dev/ashmem/dalvik*'
      THEN 'java'
    WHEN s.path GLOB '[anon:scudo*' OR s.path GLOB '[anon:libc_malloc*'
      OR s.path GLOB '[anon:jemalloc*' OR s.path GLOB '[anon:GWP-ASan*'
      OR s.path = '[heap]'
      THEN 'native'
    WHEN s.path = '[stack]' OR s.path GLOB '[anon:stack*'
      THEN 'stack'
    WHEN s.path GLOB '/dev/kgsl*' OR s.path GLOB '/dev/mali*'
      OR s.path GLOB '/dev/dri*' OR s.path GLOB '*dmabuf*'
      THEN 'graphics'
    WHEN s.path GLOB '/*'
      THEN 'file'
    ELSE 'other'
  END
`;

// File-backed mapping → bucket classification, for breaking the File-backed
// block down by path. Keys match FILE_BUCKETS. Expects the profiler_smaps row
// to be aliased `s`.
export const SMAPS_FILE_BUCKET_CASE_SQL = `
  CASE
    WHEN s.path GLOB '*.so' OR s.path GLOB '*.so.*' THEN 'so'
    WHEN s.path GLOB '*.jar' OR s.path GLOB '*.oat'
      OR s.path GLOB '*.odex' OR s.path GLOB '*.vdex'
      OR s.path GLOB '*.art' OR s.path GLOB '*.dex' THEN 'java_code'
    WHEN s.path GLOB '*.apk' OR s.path GLOB '*.ttf'
      OR s.path GLOB '*.otf' OR s.path GLOB '*.dat' THEN 'resources'
    ELSE 'other_file'
  END
`;

// Display metadata for the smaps categories, in stack/legend order. The keys
// match the `category` values produced by SMAPS_CATEGORY_CASE_SQL.
export const SMAPS_CATEGORIES = [
  {key: 'native', label: 'Native', color: '#4285f4'},
  {key: 'java', label: 'Java', color: '#f4b400'},
  {key: 'file', label: 'File-backed', color: '#34a853'},
  {key: 'graphics', label: 'Graphics', color: '#a142f4'},
  {key: 'stack', label: 'Thread stacks', color: '#26c6da'},
  {key: 'other', label: 'Other', color: '#9aa0a6'},
];

export const MEMMAP_GREY = '#9aa0a6';

// Color for a smaps category key, falling back to grey for unknown keys.
export function smapsCategoryColor(key: string): string {
  return SMAPS_CATEGORIES.find((c) => c.key === key)?.color ?? MEMMAP_GREY;
}
