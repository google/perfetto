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

import {NodeRegistry, NodeDescriptor, PreCreateContext} from './node_registry';
import {QueryNode, NodeType, QueryNodeState} from '../query_node';

describe('NodeRegistry', () => {
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

  describe('register', () => {
    it('should register a node descriptor', () => {
      const registry = new NodeRegistry();
      const descriptor: NodeDescriptor = {
        name: 'Test Node',
        description: 'A test node',
        icon: 'test-icon',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('test'),
      };

      registry.register('test-node', descriptor);

      const retrieved = registry.get('test-node');
      expect(retrieved).toBe(descriptor);
    });

    it('should allow registering multiple nodes', () => {
      const registry = new NodeRegistry();
      const descriptor1: NodeDescriptor = {
        name: 'Node 1',
        description: 'First node',
        icon: 'icon1',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('node1'),
      };
      const descriptor2: NodeDescriptor = {
        name: 'Node 2',
        description: 'Second node',
        icon: 'icon2',
        type: 'modification',
        factory: (_state: QueryNodeState) => createMockNode('node2'),
      };

      registry.register('node1', descriptor1);
      registry.register('node2', descriptor2);

      expect(registry.get('node1')).toBe(descriptor1);
      expect(registry.get('node2')).toBe(descriptor2);
    });

    it('should overwrite existing registration with same id', () => {
      const registry = new NodeRegistry();
      const descriptor1: NodeDescriptor = {
        name: 'Node 1',
        description: 'First node',
        icon: 'icon1',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('node1'),
      };
      const descriptor2: NodeDescriptor = {
        name: 'Node 1 Updated',
        description: 'Updated node',
        icon: 'icon1-updated',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('node1-updated'),
      };

      registry.register('node1', descriptor1);
      registry.register('node1', descriptor2);

      const retrieved = registry.get('node1');
      expect(retrieved).toBe(descriptor2);
      expect(retrieved?.name).toBe('Node 1 Updated');
    });

    it('should register node with optional fields', () => {
      const registry = new NodeRegistry();
      const preCreate = async (_context: PreCreateContext) => ({});
      const descriptor: NodeDescriptor = {
        name: 'Advanced Node',
        description: 'Node with optional fields',
        icon: 'advanced-icon',
        type: 'multisource',
        hotkey: 'ctrl+a',
        preCreate,
        factory: (_state: QueryNodeState) => createMockNode('advanced'),
      };

      registry.register('advanced-node', descriptor);

      const retrieved = registry.get('advanced-node');
      expect(retrieved?.hotkey).toBe('ctrl+a');
      expect(retrieved?.preCreate).toBe(preCreate);
    });
  });

  describe('get', () => {
    it('should return undefined for non-existent id', () => {
      const registry = new NodeRegistry();

      const result = registry.get('non-existent');

      expect(result).toBeUndefined();
    });

    it('should return registered descriptor', () => {
      const registry = new NodeRegistry();
      const descriptor: NodeDescriptor = {
        name: 'Test Node',
        description: 'A test node',
        icon: 'test-icon',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('test'),
      };

      registry.register('test-node', descriptor);

      const result = registry.get('test-node');
      expect(result).toBe(descriptor);
      expect(result?.name).toBe('Test Node');
    });

    it('should handle special characters in id', () => {
      const registry = new NodeRegistry();
      const descriptor: NodeDescriptor = {
        name: 'Special Node',
        description: 'Node with special id',
        icon: 'special-icon',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('special'),
      };

      registry.register('node:with:special-chars_123', descriptor);

      const result = registry.get('node:with:special-chars_123');
      expect(result).toBe(descriptor);
    });
  });

  describe('list', () => {
    it('should return empty array for empty registry', () => {
      const registry = new NodeRegistry();

      const result = registry.list();

      expect(result).toEqual([]);
    });

    it('should return all registered nodes', () => {
      const registry = new NodeRegistry();
      const descriptor1: NodeDescriptor = {
        name: 'Node 1',
        description: 'First node',
        icon: 'icon1',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('node1'),
      };
      const descriptor2: NodeDescriptor = {
        name: 'Node 2',
        description: 'Second node',
        icon: 'icon2',
        type: 'modification',
        factory: (_state: QueryNodeState) => createMockNode('node2'),
      };
      const descriptor3: NodeDescriptor = {
        name: 'Node 3',
        description: 'Third node',
        icon: 'icon3',
        type: 'multisource',
        factory: (_state: QueryNodeState) => createMockNode('node3'),
      };

      registry.register('node1', descriptor1);
      registry.register('node2', descriptor2);
      registry.register('node3', descriptor3);

      const result = registry.list();

      expect(result.length).toBe(3);
      expect(result).toContainEqual(['node1', descriptor1]);
      expect(result).toContainEqual(['node2', descriptor2]);
      expect(result).toContainEqual(['node3', descriptor3]);
    });

    it('should return tuples of [id, descriptor]', () => {
      const registry = new NodeRegistry();
      const descriptor: NodeDescriptor = {
        name: 'Test Node',
        description: 'A test node',
        icon: 'test-icon',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('test'),
      };

      registry.register('test-node', descriptor);

      const result = registry.list();

      expect(result.length).toBe(1);
      expect(result[0][0]).toBe('test-node');
      expect(result[0][1]).toBe(descriptor);
    });

    it('should reflect updates when node is re-registered', () => {
      const registry = new NodeRegistry();
      const descriptor1: NodeDescriptor = {
        name: 'Node 1',
        description: 'First node',
        icon: 'icon1',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('node1'),
      };
      const descriptor2: NodeDescriptor = {
        name: 'Node 1 Updated',
        description: 'Updated node',
        icon: 'icon1-updated',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('node1-updated'),
      };

      registry.register('node1', descriptor1);
      let result = registry.list();
      expect(result.length).toBe(1);
      expect(result[0][1].name).toBe('Node 1');

      registry.register('node1', descriptor2);
      result = registry.list();
      expect(result.length).toBe(1);
      expect(result[0][1].name).toBe('Node 1 Updated');
    });
  });

  describe('integration tests', () => {
    it('should handle full lifecycle of node registration', () => {
      const registry = new NodeRegistry();

      // Start empty
      expect(registry.list().length).toBe(0);

      // Register first node
      const descriptor1: NodeDescriptor = {
        name: 'Source Node',
        description: 'A source node',
        icon: 'source-icon',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('source'),
      };
      registry.register('source-node', descriptor1);
      expect(registry.list().length).toBe(1);
      expect(registry.get('source-node')).toBe(descriptor1);

      // Register second node
      const descriptor2: NodeDescriptor = {
        name: 'Modify Node',
        description: 'A modification node',
        icon: 'modify-icon',
        type: 'modification',
        factory: (_state: QueryNodeState) => createMockNode('modify'),
      };
      registry.register('modify-node', descriptor2);
      expect(registry.list().length).toBe(2);
      expect(registry.get('modify-node')).toBe(descriptor2);

      // Update first node
      const descriptor1Updated: NodeDescriptor = {
        name: 'Source Node Updated',
        description: 'Updated source node',
        icon: 'source-icon-updated',
        type: 'source',
        factory: (_state: QueryNodeState) => createMockNode('source-updated'),
      };
      registry.register('source-node', descriptor1Updated);
      expect(registry.list().length).toBe(2);
      expect(registry.get('source-node')).toBe(descriptor1Updated);
    });
  });
});
