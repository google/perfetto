// Copyright (C) 2025 The Android Open Source Project
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

import {
  type ColumnInfo,
  columnInfoFromSqlColumn,
  newColumnInfo,
} from './column_info';
import type {SqlColumn} from '../../dev.perfetto.SqlModules/sql_modules';
import {
  type PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../trace_processor/perfetto_sql_type';

describe('column_info utilities', () => {
  const stringType: PerfettoSqlType = {
    kind: 'string',
  };
  const intType: PerfettoSqlType = {
    kind: 'int',
  };
  const timestampType: PerfettoSqlType = {
    kind: 'timestamp',
  };

  describe('columnInfoFromSqlColumn', () => {
    it('should create ColumnInfo from SqlColumn with default unchecked', () => {
      const sqlColumn: SqlColumn = {
        name: 'id',
        type: intType,
      };

      const result = columnInfoFromSqlColumn(sqlColumn);

      expect(result.name).toBe('id');
      expect(result.type).toEqual(PerfettoSqlTypes.INT);
      expect(result.checked).toBe(false);
      expect(result.alias).toBeUndefined();
    });

    it('should create ColumnInfo with checked=true when specified', () => {
      const sqlColumn: SqlColumn = {
        name: 'name',
        type: stringType,
      };

      const result = columnInfoFromSqlColumn(sqlColumn, true);

      expect(result.name).toBe('name');
      expect(result.type).toEqual(PerfettoSqlTypes.STRING);
      expect(result.checked).toBe(true);
    });

    it('should handle timestamp type', () => {
      const sqlColumn: SqlColumn = {
        name: 'ts',
        type: timestampType,
      };

      const result = columnInfoFromSqlColumn(sqlColumn, false);

      expect(result.name).toBe('ts');
      expect(result.type).toEqual(PerfettoSqlTypes.TIMESTAMP);
      expect(result.checked).toBe(false);
    });
  });

  describe('newColumnInfo', () => {
    it('should create new ColumnInfo preserving column info', () => {
      const original: ColumnInfo = {
        name: 'id',
        checked: false,
        type: intType,
      };

      const result = newColumnInfo(original);

      expect(result.name).toBe('id');
      expect(result.type).toEqual(PerfettoSqlTypes.INT);
      expect(result.checked).toBe(false);
      // column is now a copy with name updated (not same reference)
      expect(result.name).toBe('id');
      expect(result.type).toBe(intType);
      expect(result.alias).toBeUndefined();
    });

    it('should use alias as name if present', () => {
      const original: ColumnInfo = {
        name: 'id',
        checked: false,
        type: intType,
        alias: 'identifier',
      };

      const result = newColumnInfo(original);

      expect(result.name).toBe('identifier');
      expect(result.type).toEqual(PerfettoSqlTypes.INT);
      // column.name should also be replaced with the alias so child nodes see the aliased name
      expect(result.name).toBe('identifier');
      expect(result.alias).toBeUndefined();
    });

    it('should override checked state when specified', () => {
      const original: ColumnInfo = {
        name: 'name',
        checked: false,
        type: stringType,
      };

      const result = newColumnInfo(original, true);

      expect(result.name).toBe('name');
      expect(result.checked).toBe(true);
    });

    it('should preserve checked state when not overridden', () => {
      const original: ColumnInfo = {
        name: 'name',
        checked: true,
        type: stringType,
      };

      const result = newColumnInfo(original);

      expect(result.checked).toBe(true);
    });

    it('should handle undefined checked parameter', () => {
      const original: ColumnInfo = {
        name: 'ts',
        checked: true,
        type: timestampType,
      };

      const result = newColumnInfo(original, undefined);

      expect(result.checked).toBe(true);
    });

    it('should clear alias in new column', () => {
      const original: ColumnInfo = {
        name: 'id',
        checked: false,
        type: intType,
        alias: 'identifier',
      };

      const result = newColumnInfo(original);

      expect(result.alias).toBeUndefined();
    });
  });
});
