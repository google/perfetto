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

// Display metadata (labels / colors) for the smaps categories. The path →
// category classification lives in the memscope.smaps SQL module. Used by the
// composition-over-time chart and the memory map.

// Display metadata for the smaps categories, in stack/legend order. The keys
// match the `category` values produced by memscope_smaps_category().
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
