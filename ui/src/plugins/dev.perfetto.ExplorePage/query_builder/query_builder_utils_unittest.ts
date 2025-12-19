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
  findErrors,
  findWarnings,
  queryToRun,
  isAQuery,
  hashNodeQuery,
} from './query_builder_utils';
import {Query, QueryNode, NodeType} from '../query_node';
import {QueryResponse} from '../../../components/query_table/queries';
import {SqlSourceNode} from './nodes/sources/sql_source';
import {Trace} from '../../../public/trace';
import protos from '../../../protos';

describe('query_builder_utils', () => {
  function createMockNode(nodeId: string): QueryNode {
    return {
      nodeId,
      type: NodeType.kTable,
      nextNodes: [],
      finalCols: [],
      state: {},
      validate: () => true,
      getTitle: () => 'Test',
      nodeSpecificModify: () => null,
      nodeDetails: () => ({content: null}),
      nodeInfo: () => null,
      clone: () => createMockNode(nodeId),
      getStructuredQuery: () => undefined,
      serializeState: () => ({}),
    } as QueryNode;
  }

  function createMockQueryResponse(
    overrides: Partial<QueryResponse> = {},
  ): QueryResponse {
    return {
      query: 'SELECT * FROM table',
      totalRowCount: 0,
      durationMs: 0,
      columns: [],
      rows: [],
      statementCount: 1,
      statementWithOutputCount: 1,
      lastStatementSql: 'SELECT * FROM table',
      ...overrides,
    };
  }

  describe('findErrors', () => {
    it('should return undefined when no errors exist', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };
      const response = createMockQueryResponse();

      const result = findErrors(query, response);

      expect(result).toBeUndefined();
    });

    it('should return error when query is an Error', () => {
      const queryError = new Error('Invalid query');
      const response = createMockQueryResponse();

      const result = findErrors(queryError, response);

      expect(result).toBe(queryError);
      expect(result?.message).toBe('Invalid query');
    });

    it('should return error when response has an error', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };
      const response = createMockQueryResponse({
        error: 'SQL syntax error',
      });

      const result = findErrors(query, response);

      expect(result).toBeDefined();
      expect(result?.message).toBe('SQL syntax error');
    });

    it('should prioritize query error over response error', () => {
      const queryError = new Error('Query error');
      const response = createMockQueryResponse({
        error: 'Response error',
      });

      const result = findErrors(queryError, response);

      expect(result).toBe(queryError);
      expect(result?.message).toBe('Query error');
    });

    it('should handle undefined response', () => {
      const query: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };

      const result = findErrors(query, undefined);

      expect(result).toBeUndefined();
    });

    it('should handle both query and response errors', () => {
      const queryError = new Error('Query error');
      const response = createMockQueryResponse({
        error: 'Response error',
      });

      const result = findErrors(queryError, response);

      // Query error takes precedence
      expect(result).toBe(queryError);
    });
  });

  describe('findWarnings', () => {
    it('should return undefined when no warnings exist', () => {
      const node = createMockNode('test');
      const response = createMockQueryResponse();

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should return undefined when response is undefined', () => {
      const node = createMockNode('test');

      const result = findWarnings(undefined, node);

      expect(result).toBeUndefined();
    });

    it('should return undefined when response has error', () => {
      const node = createMockNode('test');
      const response = createMockQueryResponse({
        error: 'Some error',
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should warn when last statement produces no output', () => {
      const node = createMockNode('test');
      const response = createMockQueryResponse({
        statementCount: 2,
        statementWithOutputCount: 0,
        columns: [],
      });

      const result = findWarnings(response, node);

      expect(result).toBeDefined();
      expect(result?.message).toContain(
        'The last statement must produce an output',
      );
    });

    it('should not warn when last statement produces output', () => {
      const node = createMockNode('test');
      const response = createMockQueryResponse({
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['id', 'name'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should warn for SqlSourceNode with non-module statements', () => {
      const node = new SqlSourceNode({
        sql: 'CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query: 'CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['result'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeDefined();
      expect(result?.message).toContain("Only 'INCLUDE PERFETTO MODULE");
      expect(result?.message).toContain('CREATE VIEW test AS SELECT 1');
    });

    it('should not warn for SqlSourceNode with only module includes', () => {
      const node = new SqlSourceNode({
        sql: 'INCLUDE PERFETTO MODULE android.slices; SELECT * FROM slice',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query: 'INCLUDE PERFETTO MODULE android.slices; SELECT * FROM slice',
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['id'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should handle multiple module includes correctly', () => {
      const node = new SqlSourceNode({
        sql: 'INCLUDE PERFETTO MODULE android.slices; INCLUDE PERFETTO MODULE android.frames; SELECT * FROM slice',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query:
          'INCLUDE PERFETTO MODULE android.slices; INCLUDE PERFETTO MODULE android.frames; SELECT * FROM slice',
        statementCount: 3,
        statementWithOutputCount: 1,
        columns: ['id'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should handle case-insensitive INCLUDE PERFETTO MODULE', () => {
      const node = new SqlSourceNode({
        sql: 'include perfetto module android.slices; SELECT * FROM slice',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query: 'include perfetto module android.slices; SELECT * FROM slice',
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['id'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should not warn for single statement SqlSourceNode', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query: 'SELECT * FROM slice',
        statementCount: 1,
        statementWithOutputCount: 1,
        columns: ['id'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should not warn for non-SqlSourceNode with multiple statements', () => {
      const node = createMockNode('test');
      const response = createMockQueryResponse({
        query: 'CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['result'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should handle empty statements in query split', () => {
      const node = new SqlSourceNode({
        sql: 'CREATE VIEW test AS SELECT 1;; SELECT * FROM test',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query: 'CREATE VIEW test AS SELECT 1;; SELECT * FROM test',
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['result'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeDefined();
      expect(result?.message).toContain('CREATE VIEW test AS SELECT 1');
    });

    it('should handle statements with whitespace', () => {
      const node = new SqlSourceNode({
        sql: '   INCLUDE PERFETTO MODULE android.slices   ;   SELECT * FROM slice   ',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query:
          '   INCLUDE PERFETTO MODULE android.slices   ;   SELECT * FROM slice   ',
        statementCount: 2,
        statementWithOutputCount: 1,
        columns: ['id'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeUndefined();
    });

    it('should detect non-module statements mixed with modules', () => {
      const node = new SqlSourceNode({
        sql: 'INCLUDE PERFETTO MODULE android.slices; CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        trace: {} as Trace,
      });
      const response = createMockQueryResponse({
        query:
          'INCLUDE PERFETTO MODULE android.slices; CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        statementCount: 3,
        statementWithOutputCount: 1,
        columns: ['result'],
      });

      const result = findWarnings(response, node);

      expect(result).toBeDefined();
      expect(result?.message).toContain('CREATE VIEW test AS SELECT 1');
    });
  });

  describe('integration tests', () => {
    it('should correctly identify error vs warning scenarios', () => {
      const node = new SqlSourceNode({
        sql: 'CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        trace: {} as Trace,
      });

      // Scenario 1: No issues
      const response1 = createMockQueryResponse({
        query: 'SELECT * FROM table',
        statementCount: 1,
        columns: ['id'],
      });
      const query1: Query = {
        sql: 'SELECT * FROM table',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };
      expect(findErrors(query1, response1)).toBeUndefined();
      expect(findWarnings(response1, createMockNode('test'))).toBeUndefined();

      // Scenario 2: Error in response
      const response2 = createMockQueryResponse({
        error: 'SQL error',
      });
      expect(findErrors(query1, response2)).toBeDefined();
      expect(findWarnings(response2, node)).toBeUndefined(); // No warnings when there's an error

      // Scenario 3: Warning for SqlSourceNode
      const response3 = createMockQueryResponse({
        query: 'CREATE VIEW test AS SELECT 1; SELECT * FROM test',
        statementCount: 2,
        columns: ['result'],
      });
      expect(findErrors(query1, response3)).toBeUndefined();
      expect(findWarnings(response3, node)).toBeDefined();
    });
  });

  describe('queryToRun', () => {
    it('should return "N/A" for undefined query', () => {
      expect(queryToRun(undefined)).toBe('N/A');
    });

    it('should return SQL only when no includes or preambles', () => {
      const query: Query = {
        sql: 'SELECT * FROM slice',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };
      expect(queryToRun(query)).toBe('SELECT * FROM slice');
    });

    it('should include INCLUDE statements with newlines', () => {
      const query: Query = {
        sql: 'SELECT * FROM slice',
        textproto: '',
        modules: ['android.slices', 'linux.cpu'],
        preambles: [],
        columns: [],
      };
      const result = queryToRun(query);
      expect(result).toContain('INCLUDE PERFETTO MODULE android.slices;');
      expect(result).toContain('INCLUDE PERFETTO MODULE linux.cpu;');
      expect(result).toContain('SELECT * FROM slice');
      // Should have blank line between includes and SQL
      expect(result).toMatch(/linux\.cpu;\n\nSELECT/);
    });

    it('should include preambles with newlines', () => {
      const query: Query = {
        sql: 'SELECT * FROM my_table',
        textproto: '',
        modules: [],
        preambles: [
          'CREATE PERFETTO TABLE my_table AS SELECT * FROM slice;',
          'CREATE PERFETTO VIEW my_view AS SELECT * FROM my_table;',
        ],
        columns: [],
      };
      const result = queryToRun(query);
      expect(result).toContain(
        'CREATE PERFETTO TABLE my_table AS SELECT * FROM slice;',
      );
      expect(result).toContain(
        'CREATE PERFETTO VIEW my_view AS SELECT * FROM my_table;',
      );
      expect(result).toContain('SELECT * FROM my_table');
      // Should have blank line between preambles and SQL
      expect(result).toMatch(/my_table;\n\nSELECT/);
    });

    it('should include both modules and preambles in correct order', () => {
      const query: Query = {
        sql: 'SELECT * FROM my_table',
        textproto: '',
        modules: ['android.slices'],
        preambles: ['CREATE PERFETTO TABLE my_table AS SELECT * FROM slice;'],
        columns: [],
      };
      const result = queryToRun(query);

      // Verify order by checking string indices
      const includePos = result.indexOf('INCLUDE PERFETTO MODULE');
      const preamblePos = result.indexOf('CREATE PERFETTO TABLE');
      const sqlPos = result.indexOf('SELECT * FROM my_table');

      // All should be found
      expect(includePos).toBeGreaterThanOrEqual(0);
      expect(preamblePos).toBeGreaterThan(0);
      expect(sqlPos).toBeGreaterThan(0);

      // Verify order: includes before preambles before SQL
      expect(includePos).toBeLessThan(preamblePos);
      expect(preamblePos).toBeLessThan(sqlPos);
    });
  });

  describe('isAQuery', () => {
    it('should return true for valid Query object', () => {
      const query: Query = {
        sql: 'SELECT * FROM slice',
        textproto: '',
        modules: [],
        preambles: [],
        columns: [],
      };
      expect(isAQuery(query)).toBe(true);
    });

    it('should return false for undefined', () => {
      expect(isAQuery(undefined)).toBe(false);
    });

    it('should return false for Error', () => {
      const error = new Error('Something went wrong');
      expect(isAQuery(error)).toBe(false);
    });

    it('should return false for object without sql property', () => {
      const notAQuery = {
        textproto: '',
        modules: [],
      };
      expect(isAQuery(notAQuery as unknown as Query)).toBe(false);
    });

    it('should return false for null', () => {
      expect(isAQuery(null as unknown as Query)).toBe(false);
    });
  });

  describe('hashNodeQuery', () => {
    function createMockNodeWithQuery(
      sq: protos.PerfettoSqlStructuredQuery | undefined,
    ): QueryNode {
      return {
        nodeId: 'test-node',
        type: NodeType.kTable,
        nextNodes: [],
        finalCols: [],
        getTitle: () => 'Test Node',
        validate: () => true,
        state: {},
        serializeState: () => ({}),
        nodeSpecificModify: () => null,
        nodeDetails: () => ({content: null, message: ''}),
        nodeInfo: () => null,
        clone: () => createMockNodeWithQuery(sq),
        getStructuredQuery: () => sq,
      };
    }

    it('should return Error when node returns undefined from getStructuredQuery', () => {
      const node = createMockNodeWithQuery(undefined);
      const result = hashNodeQuery(node);
      expect(result instanceof Error).toBe(true);
      if (result instanceof Error) {
        expect(result.message).toContain('returned undefined');
      }
    });

    it('should return consistent hash for same query', () => {
      const sq = new protos.PerfettoSqlStructuredQuery();
      sq.table = new protos.PerfettoSqlStructuredQuery.Table();
      sq.table.tableName = 'slice';

      const node1 = createMockNodeWithQuery(sq);
      const node2 = createMockNodeWithQuery(sq);

      const hash1 = hashNodeQuery(node1);
      const hash2 = hashNodeQuery(node2);

      expect(hash1 instanceof Error).toBe(false);
      expect(hash2 instanceof Error).toBe(false);
      expect(hash1).toBe(hash2);
    });

    it('should return different hash for different table names', () => {
      const sq1 = new protos.PerfettoSqlStructuredQuery();
      sq1.table = new protos.PerfettoSqlStructuredQuery.Table();
      sq1.table.tableName = 'slice';

      const sq2 = new protos.PerfettoSqlStructuredQuery();
      sq2.table = new protos.PerfettoSqlStructuredQuery.Table();
      sq2.table.tableName = 'sched';

      const node1 = createMockNodeWithQuery(sq1);
      const node2 = createMockNodeWithQuery(sq2);

      const hash1 = hashNodeQuery(node1);
      const hash2 = hashNodeQuery(node2);

      expect(hash1 instanceof Error).toBe(false);
      expect(hash2 instanceof Error).toBe(false);
      expect(hash1).not.toBe(hash2);
    });

    it('should detect changes in select columns', () => {
      const sq1 = new protos.PerfettoSqlStructuredQuery();
      const col1 = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      col1.columnName = 'id';
      sq1.selectColumns = [col1];

      const sq2 = new protos.PerfettoSqlStructuredQuery();
      const col2 = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      col2.columnName = 'name'; // Different column
      sq2.selectColumns = [col2];

      const node1 = createMockNodeWithQuery(sq1);
      const node2 = createMockNodeWithQuery(sq2);

      const hash1 = hashNodeQuery(node1);
      const hash2 = hashNodeQuery(node2);

      expect(hash1 instanceof Error).toBe(false);
      expect(hash2 instanceof Error).toBe(false);
      expect(hash1).not.toBe(hash2);
    });

    it('should detect changes in limit values', () => {
      const sq1 = new protos.PerfettoSqlStructuredQuery();
      sq1.limit = 100;

      const sq2 = new protos.PerfettoSqlStructuredQuery();
      sq2.limit = 200; // Different limit

      const node1 = createMockNodeWithQuery(sq1);
      const node2 = createMockNodeWithQuery(sq2);

      const hash1 = hashNodeQuery(node1);
      const hash2 = hashNodeQuery(node2);

      expect(hash1 instanceof Error).toBe(false);
      expect(hash2 instanceof Error).toBe(false);
      expect(hash1).not.toBe(hash2);
    });

    it('should handle nested queries', () => {
      const innerSq = new protos.PerfettoSqlStructuredQuery();
      innerSq.table = new protos.PerfettoSqlStructuredQuery.Table();
      innerSq.table.tableName = 'slice';

      const sq = new protos.PerfettoSqlStructuredQuery();
      sq.innerQuery = innerSq;

      const node = createMockNodeWithQuery(sq);
      const hash = hashNodeQuery(node);

      expect(hash instanceof Error).toBe(false);
      expect(typeof hash).toBe('string');
      if (typeof hash === 'string') {
        expect(hash.length).toBeGreaterThan(0);
      }
    });
  });
});
