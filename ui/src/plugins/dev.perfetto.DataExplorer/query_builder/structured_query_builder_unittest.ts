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

import protos from '../../../protos';
import {StructuredQueryBuilder} from './structured_query_builder';
import {QueryNode, NodeType} from '../query_node';
import {ColumnInfo} from './column_info';
import {PerfettoSqlType} from '../../../trace_processor/perfetto_sql_type';

describe('StructuredQueryBuilder', () => {
  describe('applyNodeColumnSelection', () => {
    const stringType: PerfettoSqlType = {kind: 'string'};
    const intType: PerfettoSqlType = {kind: 'int'};

    function createMockNode(columns: ColumnInfo[]): QueryNode {
      return {
        nodeId: 'test-node',
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: columns,
        state: {},
        validate: () => true,
        getTitle: () => 'Test',
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null}),
        nodeInfo: () => null,
        clone: () => createMockNode(columns),
        getStructuredQuery: () => undefined,
        serializeState: () => ({}),
      } as QueryNode;
    }

    function createEmptyQuery(): protos.PerfettoSqlStructuredQuery {
      return new protos.PerfettoSqlStructuredQuery();
    }

    it('should not modify query if all columns are checked', () => {
      const columns: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: true,
          column: {name: 'id', type: intType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: true,
          column: {name: 'name', type: stringType},
        },
      ];
      const node = createMockNode(columns);
      const sq = createEmptyQuery();

      StructuredQueryBuilder.applyNodeColumnSelection(sq, node);

      expect(sq.selectColumns).toEqual([]);
    });

    it('should set selectColumns when some columns are unchecked', () => {
      const columns: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: true,
          column: {name: 'id', type: intType},
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'name', type: stringType},
        },
        {
          name: 'age',
          type: 'INTEGER',
          checked: true,
          column: {name: 'age', type: intType},
        },
      ];
      const node = createMockNode(columns);
      const sq = createEmptyQuery();

      StructuredQueryBuilder.applyNodeColumnSelection(sq, node);

      expect(sq.selectColumns.length).toBe(2);
      expect(sq.selectColumns[0].columnName).toBe('id');
      expect(sq.selectColumns[1].columnName).toBe('age');
    });

    it('should include aliases when present', () => {
      const columns: ColumnInfo[] = [
        {
          name: 'id',
          type: 'INTEGER',
          checked: true,
          column: {name: 'id', type: intType},
          alias: 'identifier',
        },
        {
          name: 'name',
          type: 'STRING',
          checked: false,
          column: {name: 'name', type: stringType},
        },
      ];
      const node = createMockNode(columns);
      const sq = createEmptyQuery();

      StructuredQueryBuilder.applyNodeColumnSelection(sq, node);

      expect(sq.selectColumns.length).toBe(1);
      expect(sq.selectColumns[0].columnName).toBe('id');
      expect(sq.selectColumns[0].alias).toBe('identifier');
    });

    it('should handle empty column list', () => {
      const node = createMockNode([]);
      const sq = createEmptyQuery();

      StructuredQueryBuilder.applyNodeColumnSelection(sq, node);

      // Empty finalCols means every() returns true, so no modification
      expect(sq.selectColumns).toEqual([]);
    });

    it('should handle all columns unchecked', () => {
      const columns: ColumnInfo[] = [
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
      const node = createMockNode(columns);
      const sq = createEmptyQuery();

      StructuredQueryBuilder.applyNodeColumnSelection(sq, node);

      // No columns selected, so selectColumns should be empty
      expect(sq.selectColumns.length).toBe(0);
    });
  });
});
