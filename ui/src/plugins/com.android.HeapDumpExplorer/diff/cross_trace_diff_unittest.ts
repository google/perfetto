// Copyright (C) 2026 The Android Open Source Project
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

import {computePathKeys, pairTrees, type RawTreeRow} from './cross_trace_diff';

function node(
  id: number,
  parentId: number | null,
  name: string,
  selfSize: number,
  selfCount = 1,
  heapType: string | null = null,
): RawTreeRow {
  return {
    id,
    parent_id: parentId,
    name,
    root_type: null,
    heap_type: heapType,
    self_size: selfSize,
    self_count: selfCount,
  };
}

// node()'s 6th arg is heap_type, 7th is root_type. RawTreeRow needs both
// to test the seed-hash logic faithfully.
function rootNode(
  id: number,
  name: string,
  rootType: string | null,
  heapType: string | null = null,
): RawTreeRow {
  return {
    id,
    parent_id: null,
    name,
    root_type: rootType,
    heap_type: heapType,
    self_size: 0,
    self_count: 1,
  };
}

describe('cross-trace flamegraph diff: name-path pairing', () => {
  test('root key includes (name, heap_type, root_type)', () => {
    const k = computePathKeys([node(1, null, 'Root', 0)]);
    // Both heap_type and root_type are null on this node, but the seed
    // still emits the slots so the format stays consistent.
    expect(k.get(1)).toBe('Root\x00\x00');
  });

  test('a deeper path includes every ancestor', () => {
    const k = computePathKeys([
      node(1, null, 'Root', 0),
      node(2, 1, 'A', 10),
      node(3, 2, 'B', 5),
    ]);
    // Root\x00\x00 (name + heap_type + root_type slots) then per-level
    // separator \x01 then nodeKeyPart for each descendant (name + heap_type).
    expect(k.get(3)).toBe('Root\x00\x00\x01A\x00\x01B\x00');
  });

  test('heap_type is part of the key (same name + different heap_type ⇒ different key)', () => {
    const k = computePathKeys([
      node(1, null, 'Root', 0, 1, 'APP'),
      node(2, null, 'Root', 0, 1, 'ZYGOTE'),
    ]);
    expect(k.get(1)).not.toEqual(k.get(2));
  });

  test('root_type is part of the root key (regression for cross-product bug)', () => {
    // Two roots with identical name and heap_type but different
    // root_type used to collapse to the same path identity, which made
    // the same-trace LEFT JOIN ON path_h cross-product subtrees.
    const k = computePathKeys([
      rootNode(1, 'Root', 'ROOT_STICKY_CLASS'),
      rootNode(2, 'Root', 'ROOT_JNI_GLOBAL'),
    ]);
    expect(k.get(1)).not.toEqual(k.get(2));
  });

  test('pairing: identical trees ⇒ every node UNCHANGED (delta = 0)', () => {
    const tree: RawTreeRow[] = [
      node(1, null, 'Root', 0),
      node(2, 1, 'A', 100),
      node(3, 1, 'B', 50),
    ];
    const out = pairTrees(
      tree,
      tree.map((r) => ({...r})),
    );
    expect(out).toHaveLength(3);
    for (const r of out) {
      expect(r.delta_size).toBe(0);
      expect(r.is_new).toBe(0);
      expect(r.c_self_size).toBe(r.b_self_size);
    }
  });

  test('pairing: cur > base ⇒ positive delta', () => {
    const cur: RawTreeRow[] = [node(1, null, 'Root', 0), node(2, 1, 'A', 200)];
    const base: RawTreeRow[] = [
      node(11, null, 'Root', 0),
      node(12, 11, 'A', 50),
    ];
    const out = pairTrees(cur, base);
    const a = out.find((r) => r.name === 'A')!;
    expect(a.c_self_size).toBe(200);
    expect(a.b_self_size).toBe(50);
    expect(a.delta_size).toBe(150);
    expect(a.is_new).toBe(0);
  });

  test('pairing: cur-only node is marked is_new=1 with b_self_size=0', () => {
    const cur: RawTreeRow[] = [
      node(1, null, 'Root', 0),
      node(2, 1, 'Newcomer', 80),
    ];
    const base: RawTreeRow[] = [node(11, null, 'Root', 0)];
    const out = pairTrees(cur, base);
    const n = out.find((r) => r.name === 'Newcomer')!;
    expect(n.is_new).toBe(1);
    expect(n.b_self_size).toBe(0);
    expect(n.delta_size).toBe(80);
  });

  test('pairing: base-only node is silently dropped (mirrors same-trace SQL semantics)', () => {
    const cur: RawTreeRow[] = [node(1, null, 'Root', 0)];
    const base: RawTreeRow[] = [
      node(11, null, 'Root', 0),
      node(12, 11, 'Gone', 100),
    ];
    const out = pairTrees(cur, base);
    expect(out).toHaveLength(1);
    expect(out[0].name).toBe('Root');
  });

  test('pairing: keys match across different engine-assigned ids (the whole point)', () => {
    // Same logical tree, completely different `id` numbering — pairing
    // must still align by name-path.
    const cur: RawTreeRow[] = [
      node(100, null, 'Root', 0),
      node(101, 100, 'A', 10),
    ];
    const base: RawTreeRow[] = [node(7, null, 'Root', 0), node(8, 7, 'A', 8)];
    const out = pairTrees(cur, base);
    const a = out.find((r) => r.name === 'A')!;
    expect(a.b_self_size).toBe(8);
    expect(a.delta_size).toBe(2);
  });

  test('pairing: count is paired alongside size', () => {
    const cur: RawTreeRow[] = [
      node(1, null, 'Root', 0, 1),
      node(2, 1, 'A', 100, 5),
    ];
    const base: RawTreeRow[] = [
      node(11, null, 'Root', 0, 1),
      node(12, 11, 'A', 80, 3),
    ];
    const out = pairTrees(cur, base);
    const a = out.find((r) => r.name === 'A')!;
    expect(a.delta_count).toBe(2);
    expect(a.delta_size).toBe(20);
  });
});
