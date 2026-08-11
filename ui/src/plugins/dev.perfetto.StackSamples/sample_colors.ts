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

import {HSLColor} from '../../base/color';
import type {ColorScheme} from '../../base/color_scheme';
import {hash} from '../../base/hash';
import {GRAY, makeColorScheme} from '../../components/colorizer';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';

// Frames are colored by where they come from, consistently across all
// stack-sample surfaces: the process's own binary, a shared library, or the
// kernel. Unresolved frames are grey, matching the flamegraph.
export const CATEGORY_PROGRAM = 0;
export const CATEGORY_LIBRARY = 1;
export const CATEGORY_KERNEL = 2;
export const CATEGORY_UNKNOWN = 3;

const CATEGORY_LABELS = ['Program', 'Library', 'Kernel', 'Unknown'];
const CATEGORY_HUES = [217, 110, 30];
const SATURATION = 28;
const LIGHTNESS_BASE = 48;
const LIGHTNESS_JITTER = 8;

export function sampleCategoryLabel(category: number): string {
  return CATEGORY_LABELS[category] ?? 'Unknown';
}

const cache = new Map<string, ColorScheme>();

export function sampleColorScheme(category: number, name: string): ColorScheme {
  const hue = CATEGORY_HUES[category];
  if (hue === undefined) return GRAY;
  const key = `${category}#${name}`;
  let scheme = cache.get(key);
  if (scheme === undefined) {
    const lightness = LIGHTNESS_BASE + hash(name, LIGHTNESS_JITTER);
    scheme = makeColorScheme(new HSLColor([hue, SATURATION, lightness]));
    cache.set(key, scheme);
  }
  return scheme;
}

// SQL expression classifying a frame into the categories above from its
// mapping path and (possibly-unresolved) name. |processName| identifies the
// process's own binary (matched by path basename): without it nothing can be
// told apart as "program", so everything non-kernel becomes a library.
export function sampleCategorySqlExpr(
  mappingName: string,
  frameName: string,
  processName: string | undefined,
): string {
  const programMatch =
    processName === undefined || processName === ''
      ? 'false'
      : `${mappingName} = ${sqlValueToSqliteString(processName)}
          or ${mappingName} like ${sqlValueToSqliteString(`%/${processName}`)}`;
  return `
    case
      when ${frameName} is null
        or ${frameName} = ''
        or ${frameName} = 'unknown' then ${CATEGORY_UNKNOWN}
      when ${mappingName} like '[kernel%'
        or ${mappingName} = '/kernel'
        or ${mappingName} like '/kernel/%'
        or ${mappingName} like '%vmlinux%'
        or ${mappingName} like '%.ko' then ${CATEGORY_KERNEL}
      when ${programMatch} then ${CATEGORY_PROGRAM}
      else ${CATEGORY_LIBRARY}
    end
  `;
}
