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

import protos from '../../../../protos';

export interface EnumOption {
  value: string;
  label: string;
}

/**
 * Converts an UPPER_SNAKE_CASE enum key to a human-readable label.
 * E.g., "TIME_NANOS" -> "Time nanos", "HIGHER_IS_BETTER" -> "Higher is better"
 */
export function enumKeyToLabel(key: string): string {
  return key
    .toLowerCase()
    .split('_')
    .map((word, i) =>
      i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word,
    )
    .join(' ');
}

/**
 * Extracts enum options from a protobuf enum object.
 * Filters out UNSPECIFIED values and converts keys to human-readable labels.
 */
export function getEnumOptions(
  enumObj: Record<string, string | number>,
  excludePatterns: string[] = ['UNSPECIFIED'],
): EnumOption[] {
  const options: EnumOption[] = [];
  for (const key of Object.keys(enumObj)) {
    // Skip numeric reverse mappings and excluded patterns
    if (typeof enumObj[key] !== 'number') continue;
    if (excludePatterns.some((pattern) => key.includes(pattern))) continue;
    // Skip legacy values
    if (key.includes('LEGACY')) continue;

    options.push({
      value: key,
      label: enumKeyToLabel(key),
    });
  }
  return options;
}

/**
 * Returns metric unit options from the proto enum, plus a CUSTOM option.
 */
export function getMetricUnitOptions(): EnumOption[] {
  const options = getEnumOptions(protos.TraceMetricV2Spec.MetricUnit);
  // Add custom unit option at the end
  options.push({value: 'CUSTOM', label: 'Custom unit...'});
  return options;
}

/**
 * Returns polarity options from the proto enum.
 */
export function getPolarityOptions(): EnumOption[] {
  return getEnumOptions(protos.TraceMetricV2Spec.MetricPolarity);
}

/**
 * Returns dimension uniqueness options from the proto enum.
 */
export function getDimensionUniquenessOptions(): EnumOption[] {
  return getEnumOptions(protos.TraceMetricV2Spec.DimensionUniqueness);
}
