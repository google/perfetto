// Copyright (C) 2024 The Android Open Source Project
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

import {createGraphFromSql} from './sql_json_handler';
import {SerializedGraph, SerializedNode} from './json_handler';
import {SqlSourceSerializedState} from './query_builder/nodes/sources/sql_source';
import {NodeType} from './query_node';

describe('createGraphFromSql', () => {
  it('should create a graph from a simple SQL WITH statement', () => {
    const sql = `
      WITH a AS (SELECT 1),
           b AS (SELECT * FROM a),
           c AS (SELECT * FROM b)
      SELECT * FROM c
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(4);
    expect(graph.rootNodeIds).toEqual(['a']);

    const nodeA = graph.nodes.find((n: SerializedNode) => n.nodeId === 'a');
    const nodeB = graph.nodes.find((n: SerializedNode) => n.nodeId === 'b');
    const nodeC = graph.nodes.find((n: SerializedNode) => n.nodeId === 'c');
    const nodeOutput = graph.nodes.find(
      (n: SerializedNode) => n.nodeId === 'output',
    );

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeC).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.type).toBe(NodeType.kSqlSource);
    expect((nodeA!.state as SqlSourceSerializedState).sql).toBe('SELECT 1');
    expect((nodeA!.state as SqlSourceSerializedState).customTitle).toBe('a');
    expect(nodeA!.nextNodes).toEqual(['b']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.type).toBe(NodeType.kSqlSource);
    expect((nodeB!.state as SqlSourceSerializedState).sql).toBe(
      'SELECT * FROM $a',
    );
    expect((nodeB!.state as SqlSourceSerializedState).customTitle).toBe('b');
    expect(nodeB!.nextNodes).toEqual(['c']);
    expect(nodeB!.prevNodes).toEqual(['a']);

    expect(nodeC!.type).toBe(NodeType.kSqlSource);
    expect((nodeC!.state as SqlSourceSerializedState).sql).toBe(
      'SELECT * FROM $b',
    );
    expect((nodeC!.state as SqlSourceSerializedState).customTitle).toBe('c');
    expect(nodeC!.nextNodes).toEqual(['output']);
    expect(nodeC!.prevNodes).toEqual(['b']);

    expect(nodeOutput!.type).toBe(NodeType.kSqlSource);
    expect((nodeOutput!.state as SqlSourceSerializedState).sql).toBe(
      'SELECT * FROM $c',
    );
    expect((nodeOutput!.state as SqlSourceSerializedState).customTitle).toBe(
      'output',
    );
    expect(nodeOutput!.nextNodes).toEqual([]);
    expect(nodeOutput!.prevNodes).toEqual(['c']);
  });

  it('should throw an error for malformed SQL without SELECT', () => {
    const sql = 'WITH a AS (SELECT 1)';
    expect(() => createGraphFromSql(sql)).toThrow(
      'Malformed SQL: No SELECT statement found after WITH clause.',
    );
  });

  it('should throw an error for malformed CTE clause', () => {
    const sql = 'WITH a (SELECT 1) SELECT * FROM a';
    expect(() => createGraphFromSql(sql)).toThrow(
      'Malformed CTE clause: a (SELECT 1)',
    );
  });

  it('should handle lowercase "as"', () => {
    const sql = 'with x as (select * from slice) select * from x';
    expect(() => createGraphFromSql(sql)).not.toThrow();
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(2);
  });

  it('should handle comments', () => {
    const sql = `
      WITH a AS (
        -- This is a comment
        SELECT 1
      ),
      /* This is another comment */
      b AS (SELECT * FROM a)
      SELECT * FROM b
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(3);
  });

  it('should handle string literals with commas', () => {
    const sql = `
      WITH a AS (SELECT 'hello, world'),
           b AS (SELECT * FROM a)
      SELECT * FROM b
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(3);
  });

  it('should handle PERFETTO INCLUDE MODULE statements', () => {
    const sql = `
      PERFETTO INCLUDE MODULE android.slices;
      PERFETTO INCLUDE MODULE experimental.slices;

      WITH a AS (SELECT 1)
      SELECT * FROM a
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(2);
    expect(graph.rootNodeIds).toEqual(['a']);

    const nodeA = graph.nodes.find((n: SerializedNode) => n.nodeId === 'a');
    expect(nodeA).toBeDefined();
    expect((nodeA!.state as SqlSourceSerializedState).sql).toBe(
      'PERFETTO INCLUDE MODULE android.slices;\nPERFETTO INCLUDE MODULE experimental.slices;\nSELECT 1',
    );
  });
});
