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

import {Filter} from '../../../components/widgets/datagrid/model';
import {toNumber} from './chart_utils';

export type RawRow = Record<string, unknown>;

/**
 * Utility for applying DataGrid filters to in-memory data.
 */
export class InMemoryFilterEngine {
  /**
   * Apply filters to in-memory data.
   * Returns a new array containing only rows that pass all filters.
   */
  static apply<T extends RawRow>(
    data: readonly T[],
    filters: readonly Filter[],
  ): T[] {
    if ((filters?.length ?? 0) === 0) {
      return [...data];
    }

    return data.filter((row) => {
      for (const filter of filters) {
        if (!this.matchesFilter(row, filter)) {
          return false;
        }
      }
      return true;
    });
  }

  private static matchesFilter(row: RawRow, filter: Filter): boolean {
    const fieldValue = row[filter.field];

    switch (filter.op) {
      case 'in':
        if (!Array.isArray(filter.value)) return true;
        return new Set(filter.value.map((v) => String(v))).has(
          String(fieldValue),
        );

      case 'not in':
        if (!Array.isArray(filter.value)) return true;
        return !new Set(filter.value.map((v) => String(v))).has(
          String(fieldValue),
        );

      case 'is null':
        return fieldValue === null || fieldValue === undefined;

      case 'is not null':
        return fieldValue !== null && fieldValue !== undefined;

      default: {
        // Handle string equality operators
        if (filter.op === '=' || filter.op === '!=') {
          const strValue = String(fieldValue);
          const filterStrValue = String(filter.value);
          return filter.op === '='
            ? strValue === filterStrValue
            : strValue !== filterStrValue;
        }

        // Numeric comparison operators
        const numValue = toNumber(fieldValue);
        const filterNumValue =
          typeof filter.value === 'number'
            ? filter.value
            : Number(filter.value);

        if (numValue === undefined || isNaN(filterNumValue)) {
          return false;
        }

        switch (filter.op) {
          case '<':
            return numValue < filterNumValue;
          case '<=':
            return numValue <= filterNumValue;
          case '>':
            return numValue > filterNumValue;
          case '>=':
            return numValue >= filterNumValue;
          default:
            return true; // glob, not glob - not implemented
        }
      }
    }
  }
}

/**
 * Utility for converting DataGrid filters to SQL WHERE clauses.
 */
export class SQLFilterEngine {
  /**
   * Convert filters to SQL WHERE clause.
   * Returns empty string if no filters.
   */
  static toSQL(filters: readonly Filter[]): string {
    if ((filters?.length ?? 0) === 0) {
      return '';
    }

    const clauses: string[] = [];

    for (const filter of filters) {
      const clause = this.filterToClause(filter);
      if (clause) {
        clauses.push(clause);
      }
    }

    return clauses.length > 0 ? clauses.join(' AND ') : '';
  }

  private static filterToClause(filter: Filter): string {
    const field = filter.field;

    switch (filter.op) {
      case 'in': {
        if (!Array.isArray(filter.value) || filter.value.length === 0) {
          return '';
        }
        const values = filter.value.map((v) => this.quote(v)).join(', ');
        return `${field} IN (${values})`;
      }

      case 'not in': {
        if (!Array.isArray(filter.value) || filter.value.length === 0) {
          return '';
        }
        const values = filter.value.map((v) => this.quote(v)).join(', ');
        return `${field} NOT IN (${values})`;
      }

      case 'is null':
        return `${field} IS NULL`;

      case 'is not null':
        return `${field} IS NOT NULL`;

      case 'glob':
        return `${field} GLOB ${this.quote(filter.value)}`;

      case 'not glob':
        return `${field} NOT GLOB ${this.quote(filter.value)}`;

      default:
        // Standard comparison operators
        return `${field} ${filter.op} ${this.quote(filter.value)}`;
    }
  }

  private static quote(value: unknown): string {
    if (value === null || value === undefined) {
      return 'NULL';
    }
    if (typeof value === 'number') {
      return String(value);
    }
    // Escape single quotes by doubling them
    return `'${String(value).replace(/'/g, "''")}'`;
  }
}
