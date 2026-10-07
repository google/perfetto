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

import type {ColumnType} from '../../components/widgets/datagrid/datagrid_schema';
import type {SqlValue} from '../../trace_processor/query_result';

export interface BigtraceColumnSchema {
  readonly name: string;
  readonly type: string;
}

/**
 * Base64 -> Uint8Array. Returns undefined on malformed input so callers can
 * fall back to showing the raw string rather than crashing.
 */
export function decodeBase64(raw: string): Uint8Array<ArrayBuffer> | undefined {
  try {
    const binary = atob(raw);
    const buffer = new ArrayBuffer(binary.length);
    const bytes = new Uint8Array(buffer);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  } catch {
    return undefined;
  }
}

/**
 * Maps a backend F1/GoogleSQL type name to the Perfetto DataGrid ColumnType.
 *
 * - Quantitative ('quantitative'): INT64, UINT64, FLOAT64. Enables numeric
 *   operators (=, !=, <, <=, >, >=) and right-alignment.
 * - Text ('text'): STRING, BOOL, DATE, TIMESTAMP. Enables text operators
 *   (contains, glob, is null, is not null) and suppresses numeric comparisons
 *   (which throw on non-numeric strings in InMemoryDataSource).
 * - Undefined: BYTES and unrecognised types.
 */
export function toDataGridColumnType(type: string): ColumnType | undefined {
  switch (type) {
    case 'INT64':
    case 'UINT64':
    case 'FLOAT64':
      return 'quantitative';
    case 'STRING':
    case 'BOOL':
    case 'DATE':
    case 'TIMESTAMP':
      return 'text';
    case 'BYTES':
    default:
      return undefined;
  }
}

/**
 * Parses a raw wire string from the Bigtrace backend into a native SqlValue
 * according to its schema type.
 *
 * Never throws: unparseable values and the literal "NULL" string (produced by
 * QueryUtils for null values in proto3 JSON) become null across all types.
 */
export function parseCellValue(
  raw: string | null | undefined,
  type: string,
): SqlValue {
  if (raw === null || raw === undefined || raw === 'NULL') return null;

  switch (type) {
    case 'BYTES':
      return decodeBase64(raw) ?? raw;

    case 'INT64':
    case 'UINT64': {
      if (raw.trim() === '') return null;
      try {
        return BigInt(raw);
      } catch {
        return null;
      }
    }

    case 'FLOAT64': {
      if (raw.trim() === '') return null;
      const n = Number(raw);
      if (Number.isNaN(n) && raw !== 'NaN') return null;
      return n;
    }

    case 'BOOL':
    case 'DATE':
    case 'TIMESTAMP':
    case 'STRING':
    default:
      return raw;
  }
}
