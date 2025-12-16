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
  UIFilter,
  createFiltersProto,
  createExperimentalFiltersProto,
} from './filter';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';

describe('filter operations', () => {
  // Helper to create test filters
  const createFilter = (
    column: string,
    value: string | number,
    enabled = true,
  ): UIFilter => ({
    column,
    op: '=',
    value,
    enabled,
  });

  describe('proto generation', () => {
    const sourceCols: ColumnInfo[] = [
      {
        name: 'id',
        type: 'int',
        checked: false,
        column: {name: 'id', type: {kind: 'int'}},
      },
      {
        name: 'name',
        type: 'string',
        checked: false,
        column: {name: 'name', type: {kind: 'string'}},
      },
      {
        name: 'age',
        type: 'int',
        checked: false,
        column: {name: 'age', type: {kind: 'int'}},
      },
      {
        name: 'status',
        type: 'string',
        checked: false,
        column: {name: 'status', type: {kind: 'string'}},
      },
    ];

    describe('createFiltersProto', () => {
      it('should return undefined for empty filters', () => {
        const result = createFiltersProto([], sourceCols);
        expect(result).toBeUndefined();
      });

      it('should return undefined for undefined filters', () => {
        const result = createFiltersProto(undefined, sourceCols);
        expect(result).toBeUndefined();
      });

      it('should filter out disabled filters', () => {
        const filters: UIFilter[] = [
          createFilter('id', 1, true),
          createFilter('name', 'test', false),
          createFilter('age', 25, true),
        ];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(2);
        expect(result![0].columnName).toBe('id');
        expect(result![1].columnName).toBe('age');
      });

      it('should return undefined if all filters are disabled', () => {
        const filters: UIFilter[] = [
          createFilter('id', 1, false),
          createFilter('name', 'test', false),
        ];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toBeUndefined();
      });

      it('should create proto for string filter', () => {
        const filters: UIFilter[] = [createFilter('name', 'test')];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(1);
        expect(result![0].columnName).toBe('name');
        expect(result![0].stringRhs).toEqual(['test']);
      });

      it('should create proto for number filter', () => {
        const filters: UIFilter[] = [createFilter('id', 42)];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(1);
        expect(result![0].columnName).toBe('id');
        expect(result![0].int64Rhs).toEqual([42]);
      });

      it('should handle multiple filters', () => {
        const filters: UIFilter[] = [
          createFilter('id', 1),
          createFilter('name', 'alice'),
          createFilter('age', 30),
        ];

        const result = createFiltersProto(filters, sourceCols);

        expect(result).toHaveLength(3);
        expect(result![0].columnName).toBe('id');
        expect(result![1].columnName).toBe('name');
        expect(result![2].columnName).toBe('age');
      });
    });

    describe('createExperimentalFiltersProto', () => {
      describe('without groups', () => {
        it('should return undefined for empty filters and no groups', () => {
          const result = createExperimentalFiltersProto([], sourceCols, 'AND');

          expect(result).toBeUndefined();
        });

        it('should create AND group for multiple filters', () => {
          const filters: UIFilter[] = [
            createFilter('id', 1),
            createFilter('name', 'test'),
          ];

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'AND',
          );

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );
          expect(result!.filters).toHaveLength(2);
        });

        it('should create OR group for multiple filters', () => {
          const filters: UIFilter[] = [
            createFilter('id', 1),
            createFilter('name', 'test'),
          ];

          const result = createExperimentalFiltersProto(
            filters,
            sourceCols,
            'OR',
          );

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .OR,
          );
          expect(result!.filters).toHaveLength(2);
        });

        it('should default to AND when operator not specified', () => {
          const filters: UIFilter[] = [createFilter('id', 1)];

          const result = createExperimentalFiltersProto(filters, sourceCols);

          expect(result).toBeDefined();
          expect(result!.op).toBe(
            protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
              .AND,
          );
        });
      });
    });
  });
});
