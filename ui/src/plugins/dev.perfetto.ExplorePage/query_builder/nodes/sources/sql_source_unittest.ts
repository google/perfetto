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

import {SqlSourceNode} from './sql_source';
import {QueryNode} from '../../../query_node';
import {ColumnInfo} from '../../column_info';
import {
  createMockNode,
  createColumnInfo,
  createMockStructuredQuery,
  connectSecondary,
} from '../../testing/test_utils';
import {Trace} from '../../../../../public/trace';

describe('SqlSourceNode', () => {
  // Mock trace object for tests
  const mockTrace = {} as Trace;

  function createMockNodeWithSq(id: string, columns: ColumnInfo[]): QueryNode {
    const sq = createMockStructuredQuery(id);
    return createMockNode({
      nodeId: id,
      columns,
      getTitle: () => `Mock ${id}`,
      getStructuredQuery: () => sq,
    });
  }

  describe('constructor', () => {
    it('should initialize with default values', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      expect(node.state.sql).toBe('SELECT * FROM slice');
      expect(node.state.autoExecute).toBe(false);
      expect(node.finalCols).toEqual([]);
      expect(node.nextNodes).toEqual([]);
    });

    it('should initialize secondaryInputs with correct configuration', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM $input_0',
        trace: mockTrace,
      });

      expect(node.secondaryInputs).toBeDefined();
      expect(node.secondaryInputs.min).toBe(0);
      expect(node.secondaryInputs.max).toBe('unbounded');
      expect(node.secondaryInputs.connections.size).toBe(0);
    });

    it('should have port names following $input_N pattern', () => {
      const node = new SqlSourceNode({
        sql: '',
        trace: mockTrace,
      });

      const portNames = node.secondaryInputs.portNames;
      expect(typeof portNames).toBe('function');
      if (typeof portNames === 'function') {
        expect(portNames(0)).toBe('$input_0');
        expect(portNames(1)).toBe('$input_1');
        expect(portNames(5)).toBe('$input_5');
      }
    });
  });

  describe('inputNodesList', () => {
    it('should return empty array when no inputs connected', () => {
      const node = new SqlSourceNode({
        sql: '',
        trace: mockTrace,
      });

      expect(node.inputNodesList).toEqual([]);
    });

    it('should return connected nodes sorted by port index', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM $input_0 JOIN $input_1',
        trace: mockTrace,
      });

      const input0 = createMockNodeWithSq('node0', []);
      const input1 = createMockNodeWithSq('node1', []);
      const input2 = createMockNodeWithSq('node2', []);

      // Connect in reverse order to verify sorting
      connectSecondary(input2, node, 2);
      connectSecondary(input0, node, 0);
      connectSecondary(input1, node, 1);

      const inputs = node.inputNodesList;
      expect(inputs.length).toBe(3);
      expect(inputs[0]).toBe(input0);
      expect(inputs[1]).toBe(input1);
      expect(inputs[2]).toBe(input2);
    });
  });

  describe('getStructuredQuery', () => {
    it('should return structured query without dependencies when no inputs', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();
      expect(sq?.sql?.dependencies?.length ?? 0).toBe(0);
    });

    it('should include dependencies for connected inputs', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM $input_0',
        trace: mockTrace,
      });

      const input0 = createMockNodeWithSq('node0', [
        createColumnInfo('id', 'int'),
      ]);
      connectSecondary(input0, node, 0);

      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();
      expect(sq?.sql?.dependencies?.length).toBe(1);
      expect(sq?.sql?.dependencies?.[0].alias).toBe('input_0');
    });

    it('should include multiple dependencies with correct aliases', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT a.*, b.* FROM $input_0 a JOIN $input_1 b ON a.id = b.id',
        trace: mockTrace,
      });

      const input0 = createMockNodeWithSq('node0', [
        createColumnInfo('id', 'int'),
      ]);
      const input1 = createMockNodeWithSq('node1', [
        createColumnInfo('id', 'int'),
        createColumnInfo('name', 'string'),
      ]);

      connectSecondary(input0, node, 0);
      connectSecondary(input1, node, 1);

      const sq = node.getStructuredQuery();
      expect(sq).toBeDefined();
      expect(sq?.sql?.dependencies?.length).toBe(2);

      // Verify order matches port indices since SQL depends on correct ordering
      const aliases = sq?.sql?.dependencies?.map((d) => d.alias);
      expect(aliases).toEqual(['input_0', 'input_1']);
    });

    it('should return undefined when any input has invalid query', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM $input_0',
        trace: mockTrace,
      });

      // Create a mock node that returns undefined for getStructuredQuery
      const invalidInput = createMockNode({
        nodeId: 'invalid',
        columns: [],
        getStructuredQuery: () => undefined,
      });

      connectSecondary(invalidInput, node, 0);

      const sq = node.getStructuredQuery();
      expect(sq).toBeUndefined();
    });
  });

  describe('validate', () => {
    it('should fail validation with empty SQL', () => {
      const node = new SqlSourceNode({
        sql: '',
        trace: mockTrace,
      });

      // Empty SQL fails validation
      expect(node.validate()).toBe(false);
    });

    it('should pass validation with valid SQL', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      expect(node.validate()).toBe(true);
    });

    it('should pass validation with connected inputs', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM $input_0',
        trace: mockTrace,
      });

      const input0 = createMockNodeWithSq('node0', [
        createColumnInfo('id', 'int'),
      ]);
      connectSecondary(input0, node, 0);

      expect(node.validate()).toBe(true);
    });

    describe('statement structure validation', () => {
      it('should reject statements that do not start with SELECT', () => {
        const node = new SqlSourceNode({
          sql: 'CREATE TABLE foo AS SELECT * FROM slice',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(false);
        expect(node.state.issues?.queryError?.message).toContain('SELECT');
      });

      it('should reject multiple SELECT statements', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM slice; SELECT * FROM thread',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(false);
        expect(node.state.issues?.queryError?.message).toContain(
          'INCLUDE PERFETTO MODULE',
        );
      });

      it('should allow subqueries', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM (SELECT * FROM slice) AS sub',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow CTEs', () => {
        const node = new SqlSourceNode({
          sql: 'WITH cte AS (SELECT * FROM slice) SELECT * FROM cte',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow nested subqueries', () => {
        const node = new SqlSourceNode({
          sql: `SELECT * FROM (
            SELECT id FROM (
              SELECT id FROM slice
            ) AS inner_sub
          ) AS outer_sub`,
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow SELECT in WHERE subquery', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM slice WHERE id IN (SELECT id FROM thread)',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow SELECT in comments', () => {
        const node = new SqlSourceNode({
          sql: '-- SELECT * FROM thread\nSELECT * FROM slice',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow SELECT in string literals', () => {
        const node = new SqlSourceNode({
          sql: "SELECT 'SELECT * FROM thread' AS query FROM slice",
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow SELECT in multi-line comments', () => {
        const node = new SqlSourceNode({
          sql: '/* SELECT * FROM thread */ SELECT * FROM slice',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow UNION queries', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM slice UNION SELECT * FROM thread',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow UNION ALL queries', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM slice UNION ALL SELECT * FROM thread',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow INTERSECT queries', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT id FROM slice INTERSECT SELECT id FROM thread',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow EXCEPT queries', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT id FROM slice EXCEPT SELECT id FROM thread',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow INCLUDE PERFETTO MODULE before SELECT', () => {
        const node = new SqlSourceNode({
          sql: 'INCLUDE PERFETTO MODULE slices.slices; SELECT * FROM slice',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow multiple INCLUDE PERFETTO MODULE before SELECT', () => {
        const node = new SqlSourceNode({
          sql: `INCLUDE PERFETTO MODULE slices.slices;
                INCLUDE PERFETTO MODULE android.startup;
                SELECT * FROM slice`,
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should reject non-INCLUDE statements before SELECT', () => {
        const node = new SqlSourceNode({
          sql: 'DROP TABLE foo; SELECT * FROM slice',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(false);
        expect(node.state.issues?.queryError?.message).toContain(
          'INCLUDE PERFETTO MODULE',
        );
      });

      it('should reject INCLUDE PERFETTO MODULE after SELECT', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM slice; INCLUDE PERFETTO MODULE slices.slices',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(false);
      });

      it('should reject query without SELECT', () => {
        const node = new SqlSourceNode({
          sql: 'INCLUDE PERFETTO MODULE slices.slices',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(false);
        expect(node.state.issues?.queryError?.message).toContain('SELECT');
      });

      it('should allow trailing semicolon', () => {
        const node = new SqlSourceNode({
          sql: 'SELECT * FROM slice;',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });

      it('should allow INCLUDE with trailing semicolon', () => {
        const node = new SqlSourceNode({
          sql: 'INCLUDE PERFETTO MODULE foo; SELECT * FROM slice;',
          trace: mockTrace,
        });
        expect(node.validate()).toBe(true);
      });
    });
  });

  describe('getTitle', () => {
    it('should return "Sql source" as title', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      expect(node.getTitle()).toBe('Sql source');
    });
  });

  describe('clone', () => {
    it('should create a new node with same SQL', () => {
      const original = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      const cloned = original.clone() as SqlSourceNode;

      expect(cloned).not.toBe(original);
      expect(cloned.state.sql).toBe(original.state.sql);
      expect(cloned.nodeId).not.toBe(original.nodeId);
    });

    it('should not share state with original', () => {
      const original = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      const cloned = original.clone() as SqlSourceNode;
      cloned.state.sql = 'SELECT * FROM thread';

      expect(original.state.sql).toBe('SELECT * FROM slice');
    });
  });

  describe('serializeState', () => {
    it('should serialize SQL', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      const serialized = node.serializeState();

      expect(serialized.sql).toBe('SELECT * FROM slice');
    });

    it('should not include inputNodeIds when no inputs connected', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      const serialized = node.serializeState();

      expect(serialized.inputNodeIds).toBeUndefined();
    });

    it('should serialize input node IDs in port order', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM $input_0 JOIN $input_1',
        trace: mockTrace,
      });

      const input0 = createMockNodeWithSq('node0', []);
      const input1 = createMockNodeWithSq('node1', []);

      // Connect in reverse order
      connectSecondary(input1, node, 1);
      connectSecondary(input0, node, 0);

      const serialized = node.serializeState();

      expect(serialized.inputNodeIds).toEqual(['node0', 'node1']);
    });
  });

  describe('deserializeConnections', () => {
    it('should return empty inputNodes when no inputNodeIds', () => {
      const nodes = new Map<string, QueryNode>();

      const connections = SqlSourceNode.deserializeConnections(nodes, {
        sql: 'SELECT * FROM slice',
      });

      expect(connections.inputNodes).toEqual([]);
    });

    it('should resolve input nodes from IDs', () => {
      const node0 = createMockNodeWithSq('node0', []);
      const node1 = createMockNodeWithSq('node1', []);
      const nodes = new Map<string, QueryNode>([
        ['node0', node0],
        ['node1', node1],
      ]);

      const connections = SqlSourceNode.deserializeConnections(nodes, {
        sql: 'SELECT * FROM $input_0 JOIN $input_1',
        inputNodeIds: ['node0', 'node1'],
      });

      expect(connections.inputNodes.length).toBe(2);
      expect(connections.inputNodes[0]).toBe(node0);
      expect(connections.inputNodes[1]).toBe(node1);
    });

    it('should filter out missing nodes gracefully', () => {
      const node0 = createMockNodeWithSq('node0', []);
      const nodes = new Map<string, QueryNode>([['node0', node0]]);

      const connections = SqlSourceNode.deserializeConnections(nodes, {
        sql: 'SELECT * FROM $input_0',
        inputNodeIds: ['node0', 'missing_node'],
      });

      expect(connections.inputNodes.length).toBe(1);
      expect(connections.inputNodes[0]).toBe(node0);
    });
  });

  describe('nodeSpecificModify', () => {
    it('should render SQL editor component', () => {
      const node = new SqlSourceNode({
        sql: 'SELECT * FROM slice',
        trace: mockTrace,
      });

      const result = node.nodeSpecificModify();

      // Verify it returns a vnode with the expected structure
      // In Mithril, m('.class') creates a div with that class
      expect(result).toBeDefined();
      expect(typeof result).toBe('object');
      expect((result as {tag: string}).tag).toBe('div');
    });
  });
});
