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
  getAllNodes,
  getAllDownstreamNodes,
  getAllUpstreamNodes,
  findNodeById,
} from './graph_utils';
import {QueryNode} from '../query_node';
import {TableSourceNode} from './nodes/sources/table_source';
import {Trace} from '../../../public/trace';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';

describe('graph_utils', () => {
  let mockTrace: Trace;
  let mockSqlModules: SqlModules;

  beforeEach(() => {
    mockTrace = {
      traceInfo: {
        traceTitle: 'test_trace',
      },
    } as Trace;

    mockSqlModules = {
      listTables: () => [],
      getTable: () => null,
      listModules: () => [],
      listTablesNames: () => [],
      getModuleForTable: () => undefined,
    } as unknown as SqlModules;
  });

  // Helper to create a simple test node
  function createTestNode(): QueryNode {
    return new TableSourceNode({
      trace: mockTrace,
      sqlModules: mockSqlModules,
    }) as QueryNode;
  }

  describe('getAllNodes', () => {
    it('should return empty array for empty root nodes', () => {
      const result = getAllNodes([]);
      expect(result).toEqual([]);
    });

    it('should return single node', () => {
      const node = createTestNode();
      const result = getAllNodes([node]);
      expect(result).toHaveLength(1);
      expect(result[0]).toBe(node);
    });

    it('should traverse forward edges (nextNodes)', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      const result = getAllNodes([node1]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should traverse backward edges (primaryInput)', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.primaryInput = node2;
      node2.primaryInput = node1;

      const result = getAllNodes([node3]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should traverse backward edges (secondaryInputs)', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.secondaryInputs = {
        connections: new Map([
          [0, node1],
          [1, node2],
        ]),
        min: 2,
        max: 'unbounded',
        portNames: (portIndex: number) => `Input ${portIndex}`,
      };

      const result = getAllNodes([node3]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should handle cycles without infinite loop', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node1]; // Cycle

      const result = getAllNodes([node1]);
      expect(result).toHaveLength(2);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
    });

    it('should deduplicate nodes in complex graph', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      // Diamond pattern: 1 -> 2, 1 -> 3, 2 -> 4, 3 -> 4
      const node4 = createTestNode();
      node1.nextNodes = [node2, node3];
      node2.nextNodes = [node4];
      node3.nextNodes = [node4];

      const result = getAllNodes([node1]);
      expect(result).toHaveLength(4);
      // node4 should only appear once
      const node4Count = result.filter((n) => n === node4).length;
      expect(node4Count).toBe(1);
    });

    it('should handle multiple root nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      const result = getAllNodes([node1, node2, node3]);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });
  });

  describe('getAllDownstreamNodes', () => {
    it('should return only the node itself if no children', () => {
      const node = createTestNode();
      const result = getAllDownstreamNodes(node);
      expect(result).toHaveLength(1);
      expect(result[0]).toBe(node);
    });

    it('should return all downstream nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      const result = getAllDownstreamNodes(node1);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });

    it('should not traverse backward edges', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node2.primaryInput = node1;
      node2.nextNodes = [node3];

      const result = getAllDownstreamNodes(node2);
      expect(result).toHaveLength(2);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
      expect(result).not.toContain(node1); // Should not go backwards
    });

    it('should handle cycles', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node1];

      const result = getAllDownstreamNodes(node1);
      expect(result).toHaveLength(2);
    });
  });

  describe('getAllUpstreamNodes', () => {
    it('should return empty array if no inputs', () => {
      const node = createTestNode();
      const result = getAllUpstreamNodes(node);
      expect(result).toEqual([]);
    });

    it('should return all upstream nodes via primaryInput', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.primaryInput = node2;
      node2.primaryInput = node1;

      const result = getAllUpstreamNodes(node3);
      expect(result).toHaveLength(2);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).not.toContain(node3); // Should not include starting node
    });

    it('should return all upstream nodes via secondaryInputs', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.secondaryInputs = {
        connections: new Map([
          [0, node1],
          [1, node2],
        ]),
        min: 2,
        max: 'unbounded',
        portNames: (portIndex: number) => `Input ${portIndex}`,
      };

      const result = getAllUpstreamNodes(node3);
      expect(result).toHaveLength(2);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).not.toContain(node3);
    });

    it('should not traverse forward edges', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node2.primaryInput = node1;
      node2.nextNodes = [node3];

      const result = getAllUpstreamNodes(node2);
      expect(result).toHaveLength(1);
      expect(result).toContain(node1);
      expect(result).not.toContain(node3); // Should not go forward
    });

    it('should handle complex upstream graph', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();
      const node4 = createTestNode();

      // node4 depends on node2 and node3, which both depend on node1
      node4.secondaryInputs = {
        connections: new Map([
          [0, node2],
          [1, node3],
        ]),
        min: 2,
        max: 'unbounded',
        portNames: (portIndex: number) => `Input ${portIndex}`,
      };
      node2.primaryInput = node1;
      node3.primaryInput = node1;

      const result = getAllUpstreamNodes(node4);
      expect(result).toHaveLength(3);
      expect(result).toContain(node1);
      expect(result).toContain(node2);
      expect(result).toContain(node3);
    });
  });

  describe('findNodeById', () => {
    it('should return undefined for empty root nodes', () => {
      const result = findNodeById('1', []);
      expect(result).toBeUndefined();
    });

    it('should find node in root nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      const result = findNodeById(node1.nodeId, [node1, node2]);
      expect(result).toBe(node1);
    });

    it('should find node in downstream nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node1.nextNodes = [node2];
      node2.nextNodes = [node3];

      const result = findNodeById(node3.nodeId, [node1]);
      expect(result).toBe(node3);
    });

    it('should find node in upstream nodes', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();
      const node3 = createTestNode();

      node3.primaryInput = node2;
      node2.primaryInput = node1;

      const result = findNodeById(node1.nodeId, [node3]);
      expect(result).toBe(node1);
    });

    it('should return undefined if node not found', () => {
      const node1 = createTestNode();
      const node2 = createTestNode();

      const result = findNodeById('999', [node1, node2]);
      expect(result).toBeUndefined();
    });
  });
});
