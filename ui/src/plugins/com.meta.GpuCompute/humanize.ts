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

// Unit conversion and human-readable formatting for GPU metric values.
//
// Provides helpers that convert raw metric values (nanoseconds, bytes, Hz, etc.)
// into appropriately scaled units (e.g. 1 500 000 000 ns → 1.5 s) while
// preserving the metric's row shape so the rest of the rendering pipeline
// can stay generic.
//
// Also exposes unit-family classification ({@link unitInfo}) and cross-unit
// conversion ({@link convertUnit}) used by the baseline comparison feature.

import {Terminology} from './terminology';

// Shape of a single metric row fed into the humanization pipeline.
type RowShape = {
  metric_id: string | null;
  metric_label: string;
  metric_unit: string;
  metric_value: number | string | null;
};
// Shape of a metric table (description + array of rows).
type TableShape = {table_desc: string | null; data: RowShape[]};

// Shape of a metric section (title + array of tables).
type SectionShape = {section: string; tables: TableShape[]};

// Type guard: returns `true` when `v` is a finite number.
function isNumber(v: unknown): v is number {
  return typeof v === 'number' && Number.isFinite(v);
}

// Picks the most readable time unit for a value given in seconds.
// Returns the scaled value and the target unit string.
export function adjustSeconds(value: number): {unit: string; value: number} {
  if (value > 1) {
    return {unit: 'second', value: value};
  }
  if (value * 1_000 > 1) {
    return {unit: 'msecond', value: value * 1_000};
  }
  if (value * 1_000_000 > 1) {
    return {unit: 'usecond', value: value * 1_000_000};
  }
  return {unit: 'nsecond', value: value * 1_000_000_000};
}

// Picks the most readable SI prefix for a plain count.
// Returns the scaled value and the prefix character.
function adjustCount(value: number): {
  prefix: '' | 'K' | 'M' | 'G';
  value: number;
} {
  if (value > 1_000_000_000) {
    return {prefix: 'G', value: value / 1_000_000_000};
  }
  if (value > 1_000_000) {
    return {prefix: 'M', value: value / 1_000_000};
  }
  if (value > 1_000) {
    return {prefix: 'K', value: value / 1_000};
  }
  return {prefix: '', value: value};
}

// Picks the most readable binary-prefix unit for a byte value.
// Uses IEC powers of 1024 (KiB, MiB, GiB).
function adjustBytes(value: number): {
  unit: 'byte' | 'Kbyte' | 'Mbyte' | 'Gbyte';
  value: number;
} {
  if (value > 1_073_741_824) {
    return {unit: 'Gbyte', value: value / 1_073_741_824};
  }
  if (value > 1_048_576) {
    return {unit: 'Mbyte', value: value / 1_048_576};
  }
  if (value > 1_024) {
    return {unit: 'Kbyte', value: value / 1_024};
  }
  return {unit: 'byte', value: value};
}

// Humanizes a single metric row by converting units to human-readable form.
export function humanizeRow<T extends RowShape>(
  row: T,
  terminology?: Terminology,
): T {
  const {metric_unit: unit, metric_value: value} = row;

  // Leaving strings, nulls, undefined and % rows the same
  if (typeof value === 'string' || value == null || unit.includes('%')) {
    return row;
  }

  let humanizedUnit = unit;
  let humanizedValue: number | string | null = value;

  if (!isNumber(value)) {
    return row;
  }

  // Use current terminology's per-group denominator (defaults to 'block')
  const perGroupDenom = terminology?.block.name ?? 'block';

  switch (unit) {
    // Adjust seconds
    case 'second': {
      const adj = adjustSeconds(value);
      humanizedUnit = adj.unit;
      humanizedValue = adj.value;
      break;
    }
    case 'nsecond': {
      const adj = adjustSeconds(value / 1_000_000_000);
      humanizedUnit = adj.unit;
      humanizedValue = adj.value;
      break;
    }
    case 'cycle/second': {
      if (value === 0) {
        break;
      }
      const adj = adjustSeconds(1 / value);
      humanizedUnit = `cycle/${adj.unit}`;
      humanizedValue = 1 / adj.value;
      break;
    }
    // Adjust count
    case 'hz': {
      const adj = adjustCount(value);
      humanizedUnit = `${adj.prefix}hz`;
      humanizedValue = adj.value;
      break;
    }
    case 'element/second': {
      const adj = adjustCount(value);
      humanizedUnit = `${adj.prefix}element/second`;
      humanizedValue = adj.value;
      break;
    }
    // Adjust bytes
    case 'byte': {
      const adj = adjustBytes(value);
      humanizedUnit = adj.unit;
      humanizedValue = adj.value;
      break;
    }
    case `byte/${perGroupDenom}`: {
      const adj = adjustBytes(value);
      humanizedUnit = `${adj.unit}/${perGroupDenom}`;
      humanizedValue = adj.value;
      break;
    }
    case 'byte/second': {
      const adj = adjustBytes(value);
      humanizedUnit = `${adj.unit}/second`;
      humanizedValue = adj.value;
      break;
    }
    default:
      break;
  }

  return {
    // We copy the row and override the metric unit and value with the humanized version
    ...row,
    metric_unit: humanizedUnit,
    metric_value: humanizedValue,
  };
}

// Humanizes an array of metric sections immutably.
export function humanizeSections<Type extends SectionShape>(
  sections: readonly Type[],
  terminology?: Terminology,
): Type[] {
  // shallow-copying the section
  // shallow-copying each table in `tables`
  // replacing each row in `data` with its humanized version
  const humanizeSection = (section: Type): Type => ({
    ...section,
    // Replacing `tables` with a new array where each table is also shallow-copied and its `data` rows are humanized
    tables: section.tables.map((table) => ({
      ...table,
      data: table.data.map((row) => humanizeRow(row, terminology)),
    })),
  });

  // Humanizing each section
  return sections.map(humanizeSection);
}

// Family unit conversions used to match baseline metrics to the current selection.
export type UnitFamily =
  | 'second'
  | 'hz'
  | 'byte'
  | 'byte/second'
  | 'byte/block'
  | 'element/second'
  | 'cycle'
  | 'block'
  | 'percent'
  | 'other';

// Returns the unit family and relative scaling factor from the base unit.
export function unitInfo(
  unit: string,
  terminology?: Terminology,
): [UnitFamily, number] {
  // percent
  if (unit.includes('%')) return ['percent', 1];

  // seconds
  if (unit === 'second') return ['second', 1];
  if (unit === 'msecond') return ['second', 1e-3];
  if (unit === 'usecond') return ['second', 1e-6];
  if (unit === 'nsecond') return ['second', 1e-9];

  // hz / cycle per second
  if (unit === 'hz' || unit === 'cycle/second') return ['hz', 1];
  if (unit === 'Khz') return ['hz', 1e3];
  if (unit === 'Mhz') return ['hz', 1e6];
  if (unit === 'Ghz') return ['hz', 1e9];

  // bytes
  if (unit === 'byte') return ['byte', 1];
  if (unit === 'Kbyte') return ['byte', 1024];
  if (unit === 'Mbyte') return ['byte', 1024 * 1024];
  if (unit === 'Gbyte') return ['byte', 1024 * 1024 * 1024];

  // bytes per second
  if (unit === 'byte/second') return ['byte/second', 1];
  if (unit === 'Kbyte/second') return ['byte/second', 1024];
  if (unit === 'Mbyte/second') return ['byte/second', 1024 * 1024];
  if (unit === 'Gbyte/second') return ['byte/second', 1024 * 1024 * 1024];

  // bytes per block (per-group denominator from terminology)
  const perGroupDenom = terminology?.block.name ?? 'block';
  if (unit === `byte/${perGroupDenom}`) return ['byte/block', 1];
  if (unit === `Kbyte/${perGroupDenom}`) return ['byte/block', 1024];
  if (unit === `Mbyte/${perGroupDenom}`) {
    return ['byte/block', 1024 * 1024];
  }
  if (unit === `Gbyte/${perGroupDenom}`) {
    return ['byte/block', 1024 * 1024 * 1024];
  }

  // elements per second
  if (unit === 'element/second') return ['element/second', 1];
  if (unit === 'Kelement/second') return ['element/second', 1e3];
  if (unit === 'Melement/second') return ['element/second', 1e6];
  if (unit === 'Gelement/second') return ['element/second', 1e9];

  return ['other', 1];
}

// Converts a value from one unit to another within the same unit family.
export function convertUnit(
  value: number,
  fromUnit: string,
  toUnit: string,
  terminology?: Terminology,
): number | null {
  const [fromFamily, fromFactor] = unitInfo(fromUnit, terminology);
  const [toFamily, toFactor] = unitInfo(toUnit, terminology);

  // Unknown families: only comparable if unit strings match exactly
  if (fromFamily === 'other' || toFamily === 'other') {
    return fromUnit === toUnit ? value : null;
  }

  // Known families: must match, apply scaling when needed
  if (fromFamily !== toFamily) return null;

  // Percent: no scaling
  if (fromFamily === 'percent') return value;

  return (value * fromFactor) / toFactor;
}
