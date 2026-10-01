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

import type {WindowOwner} from './ui_hierarchy_screen';
import {ScreenState} from './ui_hierarchy_screen_state';
import {buildSfLayer, linkWmWindows, SfLayerIndex} from './ui_hierarchy_sf';
import {buildWmNodes, type WmContainerRow} from './ui_hierarchy_wm';

const OFFSCREEN_ROOT = 2147483645;

function layer(
  layerId: number,
  name: string,
  parent: number,
  args: Record<string, number> = {},
  visible = false,
) {
  return buildSfLayer({
    layerId,
    name,
    parent,
    relativeOf: -1,
    isVisible: visible,
    composition: visible ? 2 : 0,
    args: new Map(Object.entries(args)),
  });
}

const BUFFER = {
  'active_buffer.width': 10,
  'active_buffer.height': 10,
  'active_buffer.format': 1,
  'screen_bounds.left': 0,
  'screen_bounds.top': 0,
  'screen_bounds.right': 10,
  'screen_bounds.bottom': 10,
};

// One display with StatusBar (systemui) above an app window, and the SF
// layers drawing them.
function state(opts: {withSf?: boolean} = {}): ScreenState {
  const rows: WmContainerRow[] = [
    {
      token: 100,
      childIndex: 0,
      title: 'Display 0',
      containerType: 'DisplayContent',
      isVisible: true,
      args: new Map([
        ['layerId', 1],
        ['bounds.right', 10],
        ['bounds.bottom', 10],
      ]),
    },
    {
      token: 2,
      parentToken: 100,
      childIndex: 0,
      title: 'com.example/.Main',
      containerType: 'WindowState',
      isVisible: true,
      args: new Map([['layerId', 20]]),
    },
    {
      token: 3,
      parentToken: 100,
      childIndex: 1,
      title: 'StatusBar',
      containerType: 'WindowState',
      isVisible: true,
      args: new Map([['layerId', 10]]),
    },
  ];
  const nodes = buildWmNodes(rows, 7n);
  const sfLayers = new SfLayerIndex(
    opts.withSf === false
      ? []
      : [
          layer(1, 'Display 0#1', -1, {layer_stack: 0}),
          layer(20, 'aaaaaaa com.example/.Main#20', 1, {layer_stack: 0}),
          layer(21, 'VRI-com.example/.Main#21', 20, BUFFER, true),
          layer(10, 'bbbbbbb StatusBar#10', 1, {layer_stack: 0}),
          layer(11, 'VRI-StatusBar#11', 10, BUFFER, true),
          layer(OFFSCREEN_ROOT, 'Offscreen Root', -1),
        ],
  );
  const owner = (key: string, name: string): WindowOwner => ({
    key,
    name,
    source: 'layer_uid',
  });
  return new ScreenState({
    ts: 7n,
    nodes,
    sfLayers,
    hasSfData: opts.withSf !== false,
    displaySummaries: new Map([[100, {width: 10, height: 10, dpi: 420}]]),
    windowLinks: linkWmWindows(
      nodes.filter((n) => n.wm?.containerType === 'WindowState'),
      sfLayers,
    ),
    windowOwners: new Map([
      ['2', owner('upid:1', 'com.example')],
      ['3', owner('upid:2', 'com.android.systemui')],
    ]),
    uiWindows: [],
  });
}

function wm(s: ScreenState, id: string) {
  const n = s.byNodeId.get(id);
  if (n === undefined) throw new Error(`No node ${id}`);
  return n;
}

describe('ScreenState', () => {
  test('trees in each mode', () => {
    const s = state();
    const d = s.display(undefined);
    expect(d?.nodeId).toBe('100');
    expect(s.tree('processes', d, undefined).map((n) => n.nodeId)).toEqual([
      'process:upid:2',
      '3',
      'process:upid:1',
      '2',
    ]);
    expect(s.tree('wm', d, undefined).map((n) => n.nodeId)).toEqual([
      '100',
      '2',
      '3',
    ]);
    expect(s.tree('sf', d, undefined).map((n) => n.nodeId)).toEqual([
      'sf:1',
      'sf:20',
      'sf:21',
      'sf:10',
      'sf:11',
      `sf:${OFFSCREEN_ROOT}`,
    ]);
    // Trees are cached per mode, display and process.
    expect(s.tree('sf', d, undefined)).toBe(s.tree('sf', d, undefined));
  });

  test('the process filter applies to every mode', () => {
    const s = state();
    const d = s.display(undefined);
    expect(s.tree('processes', d, 'upid:2').map((n) => n.nodeId)).toEqual([
      'process:upid:2',
      '3',
    ]);
    expect(s.tree('wm', d, 'upid:2').map((n) => n.nodeId)).toEqual([
      '100',
      '3',
    ]);
    expect(s.tree('sf', d, 'upid:2').map((n) => n.nodeId)).toEqual([
      'sf:1',
      'sf:10',
      'sf:11',
    ]);
    // Unknown processes do not filter.
    expect(s.windows(d, 'upid:9').map((n) => n.nodeId)).toEqual(['2', '3']);
  });

  test('windows and layers stand for each other', () => {
    const s = state();
    expect(s.treeNodeIdOf('sf', '3')).toBe('sf:11');
    expect(s.treeNodeIdOf('wm', '3')).toBe('3');
    expect(s.windowOfLayer(11)?.name).toBe('StatusBar');
    expect(s.windowOfLayer(1)).toBeUndefined();
  });

  test('sections by node kind', () => {
    const s = state();
    const d = s.display(undefined);
    const titles = (id: string) => {
      const n =
        s.tree('processes', d, undefined).find((x) => x.nodeId === id) ??
        s.tree('sf', d, undefined).find((x) => x.nodeId === id) ??
        s.byNodeId.get(id);
      if (n === undefined) throw new Error(id);
      return s.sections(n, d).map((x) => x.title);
    };
    expect(titles('process:upid:2')).toEqual(['Process']);
    expect(titles('3')).toEqual(['Window', 'SurfaceFlinger']);
    expect(titles('sf:11')).toEqual(['SurfaceFlinger']);
    expect(titles('100')).toEqual(['Display']);
    const display = s.sections(wm(s, '100'), d)[0];
    expect(display.rows.map((r) => r.label)).toEqual([
      'Size',
      'Density',
      'Visible layers',
    ]);
  });

  test('without SurfaceFlinger data', () => {
    const s = state({withSf: false});
    const d = s.display(undefined);
    expect(s.tree('sf', d, undefined)).toEqual([]);
    expect(s.sections(wm(s, '3'), d).map((x) => x.title)).toEqual(['Window']);
    expect(s.sections(wm(s, '100'), d)[0].rows.map((r) => r.label)).toEqual([
      'Size',
      'Density',
    ]);
  });
});
