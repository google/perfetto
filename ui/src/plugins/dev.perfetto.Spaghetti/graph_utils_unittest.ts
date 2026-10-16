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
  deserializeStore,
  fnvHash,
  getManifest,
  serializeStore,
} from './graph_utils';

describe('fnvHash', () => {
  it('returns an 8-char hex string', () => {
    const h = fnvHash('hello');
    expect(h.length).toBe(8);
    expect(/^[0-9a-f]{8}$/.test(h)).toBe(true);
  });

  it('produces different hashes for different inputs', () => {
    expect(fnvHash('hello')).not.toBe(fnvHash('world'));
  });

  it('produces the same hash for the same input', () => {
    expect(fnvHash('test')).toBe(fnvHash('test'));
  });
});

describe('deserializeStore', () => {
  const allNodeTypes = [
    'from',
    'time_range',
    'select',
    'extend',
    'drop',
    'filter',
    'sort',
    'limit',
    'groupby',
    'join',
    'extract_arg',
    'interval_intersect',
    'union',
    'chart',
    'sql',
  ];

  it('validates defaultConfig for every registered node type', () => {
    for (const type of allNodeTypes) {
      const manifest = getManifest(type);
      expect(manifest).toBeDefined();
      const defaultConfig = manifest.defaultConfig();
      expect(manifest.schema.safeParse(defaultConfig).success).toBe(true);
    }
  });

  it('round-trips a valid graph with chained nodes and labels', () => {
    const input = {
      nodes: [
        {
          id: 'n1',
          type: 'from',
          x: 100,
          y: 200,
          config: {table: 'slice'},
          next: {
            id: 'n2',
            type: 'filter',
            config: {
              conditions: [{column: 'dur', op: '>', value: '1000'}],
              conjunction: 'AND',
            },
            next: {
              id: 'n3',
              type: 'limit',
              config: {limitCount: '50'},
            },
          },
        },
      ],
      labels: [
        {
          id: 'l1',
          x: 10,
          y: 20,
          width: 150,
          content: 'My note',
        },
      ],
    };

    const parsed = deserializeStore(JSON.stringify(input));
    expect(JSON.parse(serializeStore(parsed))).toEqual(input);
  });

  it('rejects unknown node types and Object.prototype keys', () => {
    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [{id: '1', type: 'nonexistent', x: 0, y: 0, config: {}}],
          labels: [],
        }),
      ),
    ).toThrow(/Unknown node type/);

    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [{id: '1', type: 'toString', x: 0, y: 0, config: {}}],
          labels: [],
        }),
      ),
    ).toThrow(/Unknown node type/);
  });

  it('rejects invalid config on root or chained nodes', () => {
    // Root node with wrong config type (number instead of string for table)
    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [{id: '1', type: 'from', x: 0, y: 0, config: {table: 123}}],
          labels: [],
        }),
      ),
    ).toThrow();

    // Chained node with invalid filter operator
    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [
            {
              id: '1',
              type: 'from',
              x: 0,
              y: 0,
              config: {table: 'slice'},
              next: {
                id: '2',
                type: 'filter',
                config: {
                  conditions: [{column: 'id', op: 'INVALID_OP', value: '1'}],
                },
              },
            },
          ],
          labels: [],
        }),
      ),
    ).toThrow();
  });

  it('rejects fake Mithril vnodes in labels and node configs', () => {
    const fakeVnode = {tag: '<', children: '<img src=x onerror=alert(1)>'};

    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [],
          labels: [{id: 'l1', x: 0, y: 0, width: 100, content: fakeVnode}],
        }),
      ),
    ).toThrow();

    expect(() =>
      deserializeStore(
        JSON.stringify({
          nodes: [
            {
              id: '1',
              type: 'sql',
              x: 0,
              y: 0,
              config: {sql: 'SELECT 1', inputPorts: [fakeVnode]},
            },
          ],
          labels: [],
        }),
      ),
    ).toThrow();
  });
});
