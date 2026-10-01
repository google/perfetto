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

import type {PropLink, PropRow} from './ui_hierarchy_props';
import {buildWmNodes} from './ui_hierarchy_wm';
import {
  buildSfLayer,
  buildSfTreeNodes,
  compositionLabel,
  countCompositions,
  decodeLayerFlags,
  decodeTransformType,
  describeVisibilityReasons,
  indexAtOrBefore,
  isInfiniteBounds,
  layerLabel,
  linkWindowLayers,
  linkWmWindows,
  pixelFormatName,
  SfLayerIndex,
  type SfLayer,
  sfLayerRows,
  windowAppUid,
  windowLayer,
  windowOfLayer,
} from './ui_hierarchy_sf';

const OFFSCREEN_ROOT = 2147483645;
const INFINITE = {
  'screen_bounds.left': -22080,
  'screen_bounds.top': -20920,
  'screen_bounds.right': 22080,
  'screen_bounds.bottom': 20920,
};
const NO_CROP = {
  'crop.left': 0,
  'crop.top': 0,
  'crop.right': -1,
  'crop.bottom': -1,
};

function layer(
  layerId: number,
  name: string,
  parent: number,
  args: Record<string, number | string> = {},
  opts: {
    visible?: boolean;
    composition?: number;
    relativeOf?: number;
    offscreen?: boolean;
  } = {},
): SfLayer {
  return buildSfLayer({
    layerId,
    name,
    parent,
    relativeOf: opts.relativeOf ?? -1,
    isVisible: opts.visible ?? false,
    composition: opts.composition ?? 0,
    args: new Map(
      Object.entries(
        opts.offscreen === true ? args : {layer_stack: 0, ...args},
      ),
    ),
  });
}

// The first SF snapshot of a foldable (outer display) trace: StatusBar
// under an insets animation leash, NotificationShade hidden with its
// buffer layer offscreen, a wallpaper drawn through a BBQ wrapper and the
// IME container with no buffer layer at all.
function snapshot(): SfLayerIndex {
  return new SfLayerIndex([
    layer(29, 'Display 0 name="Inner Display"#29', -1, INFINITE),
    layer(
      79,
      'WindowToken{9b1e3ae type=2000 android.os.BinderProxy@2e7ddb0}#79',
      29,
      INFINITE,
    ),
    layer(
      271,
      'Surface(name=ab1254f StatusBar#80)/@0x31d9daa - animation-leash of insets_animation#271',
      79,
      INFINITE,
    ),
    layer(80, 'ab1254f StatusBar#80', 271, {...INFINITE, owner_uid: 1000}),
    layer(
      81,
      'VRI-StatusBar#81',
      80,
      {
        'owner_uid': 10275,
        'flags': 0x2500,
        'z': 0,
        'active_buffer.width': 1080,
        'active_buffer.height': 133,
        'active_buffer.format': 1,
        'screen_bounds.left': 0,
        'screen_bounds.top': 0,
        'screen_bounds.right': 1080,
        'screen_bounds.bottom': 133,
        'color.r': 0,
        'color.g': 0,
        'color.b': 0,
        'color.a': 1,
        'curr_frame': 147,
        'covered_by[0]': 65,
        ...NO_CROP,
      },
      {visible: true, composition: 2},
    ),
    layer(78, '6a178a8 NotificationShade#78', 29, {
      'owner_uid': 1000,
      'flags': 1,
      'visibility_reason[0]': 'flag is hidden',
      'visibility_reason[1]': 'buffer is empty',
    }),
    layer(OFFSCREEN_ROOT, 'Offscreen Root', -1, {}, {offscreen: true}),
    layer(85, 'VRI-NotificationShade#85', OFFSCREEN_ROOT, {
      'owner_uid': 10275,
      'flags': 0x2500,
      'visibility_reason[0]': `hidden by parent ${OFFSCREEN_ROOT}`,
      'visibility_reason[1]': 'buffer is empty',
      'requested_color.r': -1,
      'color.r': -1,
      'color.g': -1,
      'color.b': -1,
      'color.a': 1,
    }),
    layer(
      39,
      'b8639ee com.google.pixel.wallpapers23.liveBirds.dynamic.DynamicCMFWallpaperService#39',
      29,
      {owner_uid: 1000},
    ),
    layer(41, 'WallpaperService-com.google.pixel.wallpapers23#41', 39, {
      owner_uid: 10199,
    }),
    layer(42, 'Wallpaper Transform wrapper#42', 41, {owner_uid: 10199}),
    layer(
      43,
      'Wallpaper BBQ wrapper#43',
      42,
      {
        'owner_uid': 10199,
        'active_buffer.width': 1080,
        'active_buffer.height': 2092,
        'active_buffer.format': 1,
      },
      {visible: true, composition: 2},
    ),
    layer(128, '3a60a49 InputMethod#128', 29, {owner_uid: 1000, flags: 4097}),
    layer(
      65,
      'VRI-ScreenDecorHwcOverlay#65',
      29,
      {owner_uid: 10275},
      {visible: true, composition: 6},
    ),
    // A layer whose relative-z parent is the wallpaper container.
    layer(
      12,
      'ImeContainer#12',
      29,
      {'corner_radii.tl': 0, 'shadow_radius': 0},
      {relativeOf: 39},
    ),
    // A solid color fill.
    layer(
      57,
      'Dim layer#57',
      29,
      {
        'color.r': 0,
        'color.g': 0,
        'color.b': 0,
        'color.a': 0.4,
        'corner_radius': 28,
      },
      {visible: true, composition: 3},
    ),
  ]);
}

const NO_CONTAINERS = new Set<number>();

function get(index: SfLayerIndex, id: number): SfLayer {
  const l = index.byId.get(id);
  if (l === undefined) throw new Error(`No layer ${id}`);
  return l;
}

describe('buildSfLayer', () => {
  test('parses buffer, bounds, flags and sentinels', () => {
    const l = get(snapshot(), 81);
    expect(l.parentId).toBe(80);
    expect(l.relativeOf).toBeUndefined();
    expect(l.ownerUid).toBe(10275);
    expect(l.buffer).toEqual({width: 1080, height: 133, format: 'RGBA_8888'});
    expect(l.screenBounds).toEqual({left: 0, top: 0, right: 1080, bottom: 133});
    expect(l.crop).toBeUndefined(); // (0, 0) - (-1, -1)
    expect(l.coveredBy).toEqual([65]);
    expect(l.frameNumber).toBe(147);
  });

  test('drops infinite bounds and unset colors', () => {
    const idx = snapshot();
    expect(get(idx, 80).screenBounds).toBeUndefined();
    expect(get(idx, 85).color).toBeUndefined();
    expect(get(idx, 81).color).toEqual({r: 0, g: 0, b: 0, a: 1});
  });

  test('keeps array args in order', () => {
    expect(get(snapshot(), 78).visibilityReasons).toEqual([
      'flag is hidden',
      'buffer is empty',
    ]);
  });

  test('roots have no parent', () => {
    const idx = snapshot();
    expect(get(idx, 29).parentId).toBeUndefined();
    expect(idx.roots.map((r) => r.id)).toEqual([29, OFFSCREEN_ROOT]);
  });
});

describe('isInfiniteBounds', () => {
  test('detects the symmetric unbounded rect only', () => {
    expect(
      isInfiniteBounds({
        left: -22080,
        top: -20920,
        right: 22080,
        bottom: 20920,
      }),
    ).toBe(true);
    expect(isInfiniteBounds({left: 0, top: 0, right: 1080, bottom: 2092})).toBe(
      false,
    );
    expect(isInfiniteBounds({left: -10, top: -10, right: 20, bottom: 20})).toBe(
      false,
    );
  });
});

describe('decoders', () => {
  test('layer flags by name, unknown bits as hex', () => {
    expect(decodeLayerFlags(0)).toEqual([]);
    expect(decodeLayerFlags(1)).toEqual(['HIDDEN']);
    expect(decodeLayerFlags(0x2500)).toEqual([
      'ENABLE_BACKPRESSURE',
      'IGNORE_DESTINATION_FRAME',
      'RECOVERABLE_FROM_BUFFER_STUFFING',
    ]);
    expect(decodeLayerFlags(4097)).toEqual([
      'HIDDEN',
      'CAN_OCCLUDE_PRESENTATION',
    ]);
    expect(decodeLayerFlags(0x10001)).toEqual(['HIDDEN', '0x10000']);
  });

  test('pixel formats', () => {
    expect(pixelFormatName(1)).toBe('RGBA_8888');
    expect(pixelFormatName(0x16)).toBe('RGBA_FP16');
    expect(pixelFormatName(999)).toBe('format 999');
  });

  test('composition labels', () => {
    const idx = snapshot();
    expect(compositionLabel(get(idx, 81))).toBe('HWC');
    expect(compositionLabel(get(idx, 65))).toBe('DISPLAY_DECORATION');
    expect(compositionLabel(get(idx, 57))).toBe('SOLID_COLOR');
    // UNSPECIFIED is omitted for invisible layers.
    expect(compositionLabel(get(idx, 78))).toBeUndefined();
    expect(
      compositionLabel(
        layer(1, 'x#1', -1, {}, {visible: true, composition: 1}),
      ),
    ).toBe('GPU');
    expect(compositionLabel(layer(1, 'x#1', -1, {}, {visible: true}))).toBe(
      'UNSPECIFIED',
    );
  });
});

describe('layer names', () => {
  test('labels are short names', () => {
    const idx = snapshot();
    expect(layerLabel(get(idx, 80))).toBe('StatusBar#80');
    expect(layerLabel(get(idx, 271))).toBe('insets_animation#271');
    expect(layerLabel(get(idx, OFFSCREEN_ROOT))).toBe('Offscreen Root');
  });

  test('visibility reasons name the hiding parent', () => {
    const idx = snapshot();
    expect(describeVisibilityReasons(get(idx, 85), idx)).toEqual([
      'hidden by parent Offscreen Root',
      'buffer is empty',
    ]);
  });
});

describe('linkWindowLayers', () => {
  test('container, buffer and leash of an animating window', () => {
    const idx = snapshot();
    const link = linkWindowLayers(idx, 80, 'StatusBar', NO_CONTAINERS);
    expect(link.container?.id).toBe(80);
    expect(link.buffer?.id).toBe(81);
    expect(link.leash?.id).toBe(271);
    expect(windowAppUid(idx, link, NO_CONTAINERS)).toBe(10275);
  });

  test('buffer through wrapper layers', () => {
    const idx = snapshot();
    const link = linkWindowLayers(
      idx,
      39,
      'com.google.pixel.wallpapers23.liveBirds.dynamic.DynamicCMFWallpaperService',
      NO_CONTAINERS,
    );
    expect(link.buffer?.id).toBe(43);
    expect(link.leash).toBeUndefined();
    expect(windowAppUid(idx, link, NO_CONTAINERS)).toBe(10199);
  });

  test('buffer reparented offscreen is found by name', () => {
    const idx = snapshot();
    const link = linkWindowLayers(idx, 78, 'NotificationShade', NO_CONTAINERS);
    expect(link.container?.id).toBe(78);
    expect(link.buffer?.id).toBe(85);
    expect(windowAppUid(idx, link, NO_CONTAINERS)).toBe(10275);
  });

  test('system windows without a buffer layer have no app uid', () => {
    const idx = snapshot();
    const link = linkWindowLayers(idx, 128, 'InputMethod', NO_CONTAINERS);
    expect(link.container?.id).toBe(128);
    expect(link.buffer).toBeUndefined();
    expect(windowAppUid(idx, link, NO_CONTAINERS)).toBeUndefined();
  });

  test('child windows are not taken for the parent buffer', () => {
    const idx = new SfLayerIndex([
      layer(1, 'aaaaaaa App#1', -1),
      layer(2, 'bbbbbbb Popup#2', 1),
      layer(
        3,
        'VRI-Popup#3',
        2,
        {
          'active_buffer.width': 10,
          'active_buffer.height': 10,
          'active_buffer.format': 1,
          'owner_uid': 10001,
        },
        {visible: true},
      ),
      layer(4, 'VRI-App#4', 1, {
        'active_buffer.width': 10,
        'active_buffer.height': 10,
        'active_buffer.format': 1,
        'owner_uid': 10002,
      }),
    ]);
    const link = linkWindowLayers(idx, 1, 'App', new Set([2]));
    expect(link.buffer?.id).toBe(4);
  });
});

describe('linkWmWindows', () => {
  test('links every window of a WM snapshot', () => {
    const nodes = buildWmNodes(
      [
        ['StatusBar', 80],
        ['NotificationShade', 78],
        ['InputMethod', 128],
      ].map(([title, layerId], i) => ({
        token: i + 1,
        childIndex: i,
        title: String(title),
        containerType: 'WindowState',
        isVisible: true,
        args: new Map([['layerId', Number(layerId)]]),
      })),
      1n,
    );
    const links = linkWmWindows(nodes, snapshot());
    expect(
      [...links].map(([id, l]) => [id, l.layers.buffer?.id, l.appUid]),
    ).toEqual([
      ['1', 81, 10275],
      ['2', 85, 10275],
      ['3', undefined, undefined],
    ]);
  });
});

describe('window layers', () => {
  const windows = () =>
    buildWmNodes(
      [
        ['StatusBar', 80],
        ['NotificationShade', 78],
        ['InputMethod', 128],
      ].map(([title, layerId], i) => ({
        token: i + 1,
        childIndex: i,
        title: String(title),
        containerType: 'WindowState',
        isVisible: true,
        args: new Map([['layerId', Number(layerId)]]),
      })),
      1n,
    );

  test('a window stands for its buffer layer, else its container', () => {
    const idx = snapshot();
    const links = linkWmWindows(windows(), idx);
    const layerOf = (id: string) => {
      const l = links.get(id);
      return l !== undefined ? windowLayer(l.layers)?.id : undefined;
    };
    expect(layerOf('1')).toBe(81);
    expect(layerOf('2')).toBe(85);
    // No buffer layer.
    expect(layerOf('3')).toBe(128);
  });

  test('a layer maps to the nearest window drawing it', () => {
    const idx = snapshot();
    const links = linkWmWindows(windows(), idx);
    expect(windowOfLayer(idx, links, 81)).toBe('1');
    expect(windowOfLayer(idx, links, 80)).toBe('1');
    // The leash.
    expect(windowOfLayer(idx, links, 271)).toBe('1');
    // A buffer layer reparented offscreen.
    expect(windowOfLayer(idx, links, 85)).toBe('2');
    expect(windowOfLayer(idx, links, 128)).toBe('3');
    // Layers outside any window.
    expect(windowOfLayer(idx, links, 29)).toBeUndefined();
    expect(windowOfLayer(idx, links, 79)).toBeUndefined();
    expect(windowOfLayer(idx, links, 999)).toBeUndefined();
  });
});

describe('sfLayerRows', () => {
  const byLabel = (rows: PropRow[]) =>
    new Map(rows.map((r) => [r.label, r.value]));
  const layer = (id: number, text: string, title = text): PropLink => ({
    text,
    title,
    target: {kind: 'layer', layerId: id},
  });

  test('buffer layer of a visible window', () => {
    const idx = snapshot();
    const link = linkWindowLayers(idx, 80, 'StatusBar', NO_CONTAINERS);
    const rows = sfLayerRows(idx, get(idx, 81), link);
    expect(rows.map((r) => r.label)).toEqual([
      'Layer',
      'Container',
      'Leash',
      'Visible',
      'Covered by',
      'Composition',
      'Flags',
      'Buffer',
      'Screen bounds',
      'Z',
      'Frame',
    ]);
    const m = byLabel(rows);
    expect(m.get('Layer')).toEqual({
      kind: 'links',
      links: [layer(81, 'VRI-StatusBar#81')],
    });
    expect(m.get('Container')).toEqual({
      kind: 'links',
      links: [layer(80, 'StatusBar#80', 'ab1254f StatusBar#80')],
    });
    expect(m.get('Leash')).toMatchObject({
      links: [{text: 'insets_animation#271', target: {layerId: 271}}],
    });
    expect(m.get('Covered by')).toMatchObject({
      links: [{text: 'VRI-ScreenDecorHwcOverlay#65', target: {layerId: 65}}],
    });
    expect(m.get('Composition')).toEqual({kind: 'chip', text: 'HWC'});
    expect(m.get('Buffer')).toMatchObject({
      text: '1080\u00d7133 \u00b7 RGBA_8888',
    });
    expect(m.get('Flags')).toMatchObject({
      text: 'ENABLE_BACKPRESSURE | IGNORE_DESTINATION_FRAME | RECOVERABLE_FROM_BUFFER_STUFFING',
      title: '0x2500',
    });
    // No sentinel values leak into the rows.
    expect(JSON.stringify(rows)).not.toContain('-1');
  });

  test('hidden layer lists reasons and no composition', () => {
    const idx = snapshot();
    const m = byLabel(sfLayerRows(idx, get(idx, 85)));
    // Outside a window there are no window layer links.
    expect(m.has('Layer')).toBe(false);
    expect(m.get('Visible')).toMatchObject({text: 'false'});
    expect(m.get('Invisible due to')).toEqual({
      kind: 'list',
      items: ['hidden by parent Offscreen Root', 'buffer is empty'],
    });
    expect(m.has('Composition')).toBe(false);
    expect(m.has('Color')).toBe(false);
    expect(m.has('Screen bounds')).toBe(false);
  });

  test('non-default effects only', () => {
    const idx = snapshot();
    const dim = byLabel(sfLayerRows(idx, get(idx, 57)));
    expect(dim.get('Opacity')).toMatchObject({text: '0.4'});
    expect(dim.get('Color')).toMatchObject({text: 'rgba(0, 0, 0, 0.4)'});
    expect(dim.get('Corner radius')).toMatchObject({text: '28 px'});
    const ime = byLabel(sfLayerRows(idx, get(idx, 12)));
    expect(ime.has('Corner radius')).toBe(false);
    expect(ime.has('Shadow radius')).toBe(false);
    expect(ime.has('Opacity')).toBe(false);
    expect(ime.get('Relative to')).toMatchObject({
      links: [
        {
          text: 'DynamicCMFWallpaperService#39',
          target: {layerId: 39},
        },
      ],
    });
    // A single layer stack is not worth a row.
    expect(ime.has('Layer stack')).toBe(false);
  });

  test('links to layers missing from the snapshot', () => {
    const idx = snapshot();
    const l = {...get(idx, 81), coveredBy: [999]};
    expect(byLabel(sfLayerRows(idx, l)).get('Covered by')).toEqual({
      kind: 'links',
      links: [layer(999, '#999', 'Layer 999 (not in this snapshot)')],
    });
  });
});

describe('snapshot helpers', () => {
  test('composition counts of visible layers', () => {
    expect(countCompositions([...snapshot().byId.values()])).toEqual({
      visible: 4,
      hwc: 4,
      gpu: 0,
    });
  });

  test('roots on a layer stack skip offscreen layers', () => {
    const idx = snapshot();
    expect(idx.rootsOnStack(0).map((l) => l.id)).toEqual([29]);
    expect(idx.rootsOnStack(undefined).map((l) => l.id)).toEqual([29]);
  });

  test('tree nodes in paint order with parent links', () => {
    const idx = snapshot();
    const nodes = buildSfTreeNodes(idx, [get(idx, 79)], 5n);
    expect(nodes.map((n) => n.nodeId)).toEqual([
      'sf:79',
      'sf:271',
      'sf:80',
      'sf:81',
    ]);
    expect(nodes[0].parentNodeId).toBeUndefined();
    expect(nodes[3].parentNodeId).toBe('sf:80');
    expect(nodes[3].sfLayerId).toBe(81);
    expect(nodes[3].boundsBottom).toBe(133);
  });

  test('display roots follow the display layer stack', () => {
    const idx = new SfLayerIndex([
      layer(1, 'Display 0#1', -1, {layer_stack: 0}),
      layer(2, 'WindowToken#2', -1, {layer_stack: 0}),
      layer(3, 'Display 10#3', -1, {layer_stack: 10}),
      layer(OFFSCREEN_ROOT, 'Offscreen Root', -1, {}, {offscreen: true}),
    ]);
    expect(idx.displayRoots(1).map((l) => l.id)).toEqual([1, 2]);
    expect(idx.displayRoots(3).map((l) => l.id)).toEqual([3]);
    // Without a display layer id: every on-screen root.
    expect(idx.displayRoots(undefined).map((l) => l.id)).toEqual([1, 2, 3]);
    // A display layer missing from the snapshot draws nothing.
    expect(idx.displayRoots(99)).toEqual([]);
    expect(idx.offscreenRoots.map((l) => l.id)).toEqual([OFFSCREEN_ROOT]);
  });

  test('layers with their ancestors and descendants', () => {
    const idx = snapshot();
    expect([...idx.withContext([80])].sort((a, b) => a - b)).toEqual([
      29, 79, 80, 81, 271,
    ]);
    expect([...idx.withContext([85, 999])].sort((a, b) => a - b)).toEqual([
      85,
      OFFSCREEN_ROOT,
    ]);
  });

  test('tree nodes restricted to kept layers', () => {
    const idx = snapshot();
    const keep = idx.withContext([81, 85]);
    const nodes = buildSfTreeNodes(
      idx,
      [...idx.displayRoots(29), ...idx.offscreenRoots],
      5n,
      keep,
    );
    expect(nodes.map((n) => n.nodeId)).toEqual([
      'sf:29',
      'sf:79',
      'sf:271',
      'sf:80',
      'sf:81',
      `sf:${OFFSCREEN_ROOT}`,
      'sf:85',
    ]);
    expect(nodes[1].childIndex).toBe(0);
  });

  test('snapshot at or before a timestamp', () => {
    const snaps = [{ts: 10n}, {ts: 20n}, {ts: 30n}];
    expect(indexAtOrBefore(snaps, 5n)).toBe(0);
    expect(indexAtOrBefore(snaps, 20n)).toBe(1);
    expect(indexAtOrBefore(snaps, 29n)).toBe(1);
    expect(indexAtOrBefore(snaps, 99n)).toBe(2);
  });
});

describe('decodeTransformType', () => {
  test('matrix kinds', () => {
    expect(decodeTransformType(0x1)).toEqual(['TRANSLATE']);
    expect(decodeTransformType(0x5)).toEqual(['SCALE', 'TRANSLATE']);
  });

  test('orientations', () => {
    expect(decodeTransformType(0x401)).toEqual(['ROT_90', 'TRANSLATE']);
    expect(decodeTransformType(0x300)).toEqual(['ROT_180']);
    expect(decodeTransformType(0x702)).toEqual(['ROT_270', 'ROTATE']);
    expect(decodeTransformType(0x100)).toEqual(['FLIP_H']);
    expect(decodeTransformType(0x8008)).toEqual(['ROT_INVALID', '0x8']);
  });
});
