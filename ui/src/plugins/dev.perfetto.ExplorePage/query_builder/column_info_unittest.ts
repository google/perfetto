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
  ColumnInfo,
  columnInfoFromSqlColumn,
  columnInfoFromName,
  newColumnInfo,
  newColumnInfoList,
} from './column_info';
import {SqlColumn} from '../../dev.perfetto.SqlModules/sql_modules';
import {PerfettoSqlType} from '../../../trace_processor/perfetto_sql_type';

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
      expect(result.type).toBe('INT');
      expect(result.checked).toBe(false);
      expect(result.column).toBe(sqlColumn);
      expect(result.alias).toBeUndefined();
    });

    it('should create ColumnInfo with checked=true when specified', () => {
      const sqlColumn: SqlColumn = {
        name: 'name',
        type: stringType,
      };

      const result = columnInfoFromSqlColumn(sqlColumn, true);

      expect(result.name).toBe('name');
      expect(result.type).toBe('STRING');
      expect(result.checked).toBe(true);
      expect(result.column).toBe(sqlColumn);
    });

    it('should handle timestamp type', () => {
      const sqlColumn: SqlColumn = {
        name: 'ts',
        type: timestampType,
      };

      const result = columnInfoFromSqlColumn(sqlColumn, false);

      expect(result.name).toBe('ts');
      expect(result.type).toBe('TIMESTAMP');
      expect(result.checked).toBe(false);
    });
  });

  describe('columnInfoFromName', () => {
    it('should create ColumnInfo from name with default unchecked', () => {
      const result = columnInfoFromName('test_column');

      expect(result.name).toBe('test_column');
      expect(result.type).toBe('NA');
      expect(result.checked).toBe(false);
      expect(result.column.name).toBe('test_column');
      expect(result.column.type).toBe(undefined);
    });

    it('should create ColumnInfo with checked=true when specified', () => {
      const result = columnInfoFromName('another_column', true);

      expect(result.name).toBe('another_column');
      expect(result.type).toBe('NA');
      expect(result.checked).toBe(true);
    });

    it('should handle empty name', () => {
      const result = columnInfoFromName('');

      expect(result.name).toBe('');
      expect(result.type).toBe('NA');
      expect(result.checked).toBe(false);
    });
  });

  describe('newColumnInfo', () => {
    it('should create new ColumnInfo preserving column info', () => {
      const original: ColumnInfo = {
        name: 'id',
        type: 'INTEGER',
        checked: false,
        column: {name: 'id', type: intType},
      };

      const result = newColumnInfo(original);

      expect(result.name).toBe('id');
      expect(result.type).toBe('INT');
      expect(result.checked).toBe(false);
      // column is now a copy with name updated (not same reference)
      expect(result.column.name).toBe('id');
      expect(result.column.type).toBe(intType);
      expect(result.alias).toBeUndefined();
    });

    it('should use alias as name if present', () => {
      const original: ColumnInfo = {
        name: 'id',
        type: 'INTEGER',
        checked: false,
        column: {name: 'id', type: intType},
        alias: 'identifier',
      };

      const result = newColumnInfo(original);

      expect(result.name).toBe('identifier');
      expect(result.type).toBe('INT');
      // column.name should also be replaced with the alias so child nodes see the aliased name
      expect(result.column.name).toBe('identifier');
      expect(result.alias).toBeUndefined();
    });

    it('should override checked state when specified', () => {
      const original: ColumnInfo = {
        name: 'name',
        type: 'STRING',
        checked: false,
        column: {name: 'name', type: stringType},
      };

      const result = newColumnInfo(original, true);

      expect(result.name).toBe('name');
      expect(result.checked).toBe(true);
    });

    it('should preserve checked state when not overridden', () => {
      const original: ColumnInfo = {
        name: 'name',
        type: 'STRING',
        checked: true,
        column: {name: 'name', type: stringType},
      };

      const result = newColumnInfo(original);

      expect(result.checked).toBe(true);
    });

    it('should handle undefined checked parameter', () => {
      const original: ColumnInfo = {
        name: 'ts',
        type: 'TIMESTAMP_NS',
        checked: true,
        column: {name: 'ts', type: timestampType},
      };

      const result = newColumnInfo(original, undefined);

      expect(result.checked).toBe(true);
    });

    it('should clear alias in new column', () => {
      const original: ColumnInfo = {
        name: 'id',
        type: 'INTEGER',
        checked: false,
        column: {name: 'id', type: intType},
        alias: 'identifier',
      };

      const result = newColumnInfo(original);

      expect(result.alias).toBeUndefined();
    });
  });

  describe('newColumnInfoList', () => {
    it('should create new list preserving all columns', () => {
      const original: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: false,
          column: {name: 'id', type: intType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'name', type: stringType},
        },
        {
          name: 'ts',
          type: 'TIMESTAMP_NS',
          checked: true,
          column: {name: 'ts', type: timestampType},
        },
      ];

      const result = newColumnInfoList(original);

      expect(result.length).toBe(3);
      expect(result[0].name).toBe('id');
      expect(result[0].checked).toBe(false);
      expect(result[1].name).toBe('name');
      expect(result[1].checked).toBe(false);
      expect(result[2].name).toBe('ts');
      expect(result[2].checked).toBe(true);
    });

    it('should override checked state for all columns when specified', () => {
      const original: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: false,
          column: {name: 'id', type: intType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'name', type: stringType},
        },
      ];

      const result = newColumnInfoList(original, true);

      expect(result.length).toBe(2);
      expect(result[0].checked).toBe(true);
      expect(result[1].checked).toBe(true);
    });

    it('should handle empty list', () => {
      const result = newColumnInfoList([]);

      expect(result).toEqual([]);
    });

    it('should handle aliases', () => {
      const original: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: false,
          column: {name: 'id', type: intType},
          alias: 'identifier',
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'full_name', type: stringType},
          alias: 'name',
        },
      ];

      const result = newColumnInfoList(original);

      expect(result.length).toBe(2);
      expect(result[0].name).toBe('identifier');
      expect(result[0].alias).toBeUndefined();
      expect(result[1].name).toBe('name');
      expect(result[1].alias).toBeUndefined();
    });

    it('should create independent copies', () => {
      const original: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: false,
          column: {name: 'id', type: intType},
        },
      ];

      const result = newColumnInfoList(original, true);

      expect(original[0].checked).toBe(false);
      expect(result[0].checked).toBe(true);
    });
  });
});
