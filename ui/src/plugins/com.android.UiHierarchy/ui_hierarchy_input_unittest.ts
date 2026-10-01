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

import type {UiHierarchyNode} from './ui_hierarchy_data';
import {
  buildSfInput,
  decodeInputConfig,
  inputCanvas,
  inputRows,
  type SfInput,
  type SfInputRow,
  transformRect,
  windowInputLayer,
} from './ui_hierarchy_input';
import type {PropRow} from './ui_hierarchy_props';
import {buildSfLayer, type SfLayer} from './ui_hierarchy_sf';

const IDENTITY = {dsdx: 1, dtdx: 0, tx: 0, dtdy: 0, dsdy: 1, ty: 0};

function input(overrides: Partial<SfInput> = {}): SfInput {
  return {
    frame: {left: 0, top: 0, right: 10, bottom: 10},
    touchable: [{left: 0, top: 0, right: 10, bottom: 10}],
    depth: 0,
    isVisible: true,
    isSpy: false,
    focusable: true,
    layoutParamsType: 0,
    config: 0,
    ...overrides,
  };
}

function layer(id: number, inp?: SfInput): SfLayer {
  const l = buildSfLayer({
    layerId: id,
    name: `layer${id}`,
    parent: -1,
    relativeOf: -1,
    isVisible: true,
    composition: 2,
    args: new Map(),
  });
  return inp === undefined ? l : {...l, input: inp};
}

function node(nodeId: string): UiHierarchyNode {
  return {
    rowId: 0,
    ts: 0n,
    dur: 0n,
    windowId: 1,
    nodeId,
    childIndex: 0,
    kind: 1,
    kindName: 'View',
    name: `n${nodeId}`,
    boundsLeft: 0,
    boundsTop: 0,
    boundsRight: 1,
    boundsBottom: 1,
    alpha: 1,
    flags: 0n,
    isVisible: true,
    isClickable: false,
    isFocused: false,
    isTextRedacted: false,
  };
}

function text(rows: ReadonlyArray<PropRow>, label: string): string | undefined {
  const v = rows.find((r) => r.label === label)?.value;
  if (v === undefined) return undefined;
  if (v.kind === 'text') return v.text;
  if (v.kind === 'list') return v.items.join('; ');
  return undefined;
}

describe('transformRect', () => {
  test('translates', () => {
    expect(
      transformRect({x: 0, y: 0, w: 1080, h: 63}, {...IDENTITY, ty: 2029}),
    ).toEqual({left: 0, top: 2029, right: 1080, bottom: 2092});
  });

  test('without a transform keeps the rect', () => {
    expect(transformRect({x: 1, y: 2, w: 3, h: 4})).toEqual({
      left: 1,
      top: 2,
      right: 4,
      bottom: 6,
    });
  });

  test('rotation gives the bounding box', () => {
    // 90 degrees: (x, y) -> (-y + 100, x).
    const rot = {dsdx: 0, dtdx: -1, tx: 100, dtdy: 1, dsdy: 0, ty: 0};
    expect(transformRect({x: 0, y: 0, w: 20, h: 10}, rot)).toEqual({
      left: 90,
      top: 0,
      right: 100,
      bottom: 20,
    });
  });

  test('empty is undefined', () => {
    expect(transformRect({x: 0, y: 0, w: 0, h: 0})).toBeUndefined();
    expect(transformRect({x: 5, y: 5, w: 10, h: -1})).toBeUndefined();
  });
});

describe('decodeInputConfig', () => {
  test('known flags in bit order', () => {
    expect(decodeInputConfig(260)).toEqual([
      'NOT_FOCUSABLE',
      'TRUSTED_OVERLAY',
    ]);
    expect(decodeInputConfig(0)).toEqual([]);
  });

  test('unknown bits as one hex value', () => {
    expect(decodeInputConfig((1 << 14) | (1 << 20) | (1 << 21))).toEqual([
      'SPY',
      '0x300000',
    ]);
  });
});

describe('buildSfInput', () => {
  const row: SfInputRow = {
    layerId: 7,
    frame: {x: 0, y: 0, w: 1080, h: 63},
    transform: {...IDENTITY, ty: 2029},
    region: [
      {x: 0, y: 0, w: 1080, h: 30},
      {x: 0, y: 0, w: 0, h: 0},
    ],
    depth: 3,
    isVisible: true,
    isSpy: false,
    focusable: false,
    layoutParamsType: 2000,
    config: 4,
  };

  test('maps frame and touchable region to display space', () => {
    const inp = buildSfInput(row);
    expect(inp.frame).toEqual({left: 0, top: 2029, right: 1080, bottom: 2092});
    expect(inp.touchable).toEqual([
      {left: 0, top: 2029, right: 1080, bottom: 2059},
    ]);
    expect(inp.depth).toBe(3);
    expect(inp.layoutParamsType).toBe(2000);
  });

  test('empty frame is undefined', () => {
    const inp = buildSfInput({...row, frame: {x: 0, y: 0, w: 0, h: 0}});
    expect(inp.frame).toBeUndefined();
  });
});

describe('inputRows', () => {
  test('all rows', () => {
    const rows = inputRows(
      input({focusable: false, layoutParamsType: 2000, config: 260}),
      true,
    );
    expect(rows.map((r) => r.label)).toEqual([
      'Frame',
      'Touchable',
      'Visible',
      'Focusable',
      'Type',
      'Config',
    ]);
    expect(text(rows, 'Frame')).toBe('[0, 0, 10, 10]  10 x 10 px');
    expect(text(rows, 'Focusable')).toBe('false');
    expect(text(rows, 'Type')).toBe('STATUS_BAR (2000)');
    expect(text(rows, 'Config')).toBe('NOT_FOCUSABLE | TRUSTED_OVERLAY');
    const config = rows.find((r) => r.label === 'Config')?.value;
    expect(config?.kind === 'text' && config.title).toBe('0x104');
  });

  test('no touchable region reads none', () => {
    expect(text(inputRows(input({touchable: []}), false), 'Touchable')).toBe(
      'none',
    );
  });

  test('drops type without withType, and empty frame, type and config', () => {
    const labels = (inp: SfInput, withType: boolean) =>
      inputRows(inp, withType).map((r) => r.label);
    expect(labels(input({layoutParamsType: 2000}), false)).not.toContain(
      'Type',
    );
    expect(labels(input({frame: undefined}), true)).toEqual([
      'Touchable',
      'Visible',
      'Focusable',
    ]);
  });
});

describe('windowInputLayer', () => {
  test('prefers the buffer layer, then container, then leash', () => {
    const buffer = layer(1, input());
    const container = layer(2, input());
    const leash = layer(3, input());
    expect(windowInputLayer({buffer, container, leash})).toBe(buffer);
    expect(windowInputLayer({buffer: layer(1), container, leash})).toBe(
      container,
    );
    expect(windowInputLayer({container: layer(2), leash})).toBe(leash);
    expect(windowInputLayer({buffer: layer(1)})).toBeUndefined();
  });
});

describe('inputCanvas', () => {
  test('orders by depth, top layer per node wins, skips the rest', () => {
    const layers = [
      layer(
        1,
        input({depth: 5, frame: {left: 0, top: 0, right: 5, bottom: 5}}),
      ),
      layer(2, input({depth: 1, isVisible: false, touchable: []})),
      // Same node as layer 1, lower: dropped.
      layer(3, input({depth: 2})),
      // No node.
      layer(4, input({depth: 9})),
      // No frame.
      layer(5, input({depth: 7, frame: undefined})),
      // No input.
      layer(6),
    ];
    const nodes = new Map([
      [1, node('a')],
      [2, node('b')],
      [3, node('a')],
      [5, node('c')],
      [6, node('d')],
    ]);
    const c = inputCanvas(layers, (l) => nodes.get(l.id));
    expect(c.nodes.map((n) => n.nodeId)).toEqual(['b', 'a']);
    const [b, a] = c.nodes;
    expect([a.boundsLeft, a.boundsTop, a.boundsRight, a.boundsBottom]).toEqual([
      0, 0, 5, 5,
    ]);
    expect(b.isVisible).toBe(false);
    expect(c.touchable.get('b')).toEqual([]);
    expect(c.touchable.get('a')).toHaveLength(1);
    // The tree nodes are left alone.
    expect(nodes.get(1)?.boundsRight).toBe(1);
  });
});
