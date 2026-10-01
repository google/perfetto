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

// Input windows: the InputWindowInfo of SF layers. Trace processor computes
// each input window's frame and touchable region in layer space plus the
// layer transform (the same rects Winscope draws); here they are mapped to
// display space for the canvas and the Input properties.

import type {Engine} from '../../trace_processor/engine';
import {NUM, NUM_NULL} from '../../trace_processor/query_result';
import type {UiHierarchyNode} from './ui_hierarchy_data';
import {
  formatBounds,
  type PropRow,
  PropRowsBuilder,
} from './ui_hierarchy_props';
import type {
  SfLayer,
  SfRect,
  SfSnapshot,
  SfWindowLayers,
} from './ui_hierarchy_sf';
import {windowTypeToString} from './ui_hierarchy_wm';

export interface SfInput {
  // Display space; undefined when empty.
  readonly frame?: SfRect;
  // Where the window takes touches, display space. Empty: nowhere.
  readonly touchable: ReadonlyArray<SfRect>;
  // Input z-order on the layer's display, bottom first.
  readonly depth: number;
  readonly isVisible: boolean;
  readonly isSpy: boolean;
  readonly focusable: boolean;
  // WindowManager.LayoutParams type; 0 when unset.
  readonly layoutParamsType: number;
  // android.os.InputConfig flags.
  readonly config: number;
}

// A rect as stored by trace processor (x, y, w, h).
export interface XywhRect {
  readonly x: number;
  readonly y: number;
  readonly w: number;
  readonly h: number;
}

// Row-major 2x3 affine transform, as in android_winscope_transform.
export interface Affine {
  readonly dsdx: number;
  readonly dtdx: number;
  readonly tx: number;
  readonly dtdy: number;
  readonly dsdy: number;
  readonly ty: number;
}

export interface SfInputRow {
  readonly layerId: number;
  readonly frame: XywhRect;
  readonly transform?: Affine;
  readonly region: ReadonlyArray<XywhRect>;
  readonly depth: number;
  readonly isVisible: boolean;
  readonly isSpy: boolean;
  readonly focusable: boolean;
  readonly layoutParamsType: number;
  readonly config: number;
}

// frameworks/native/libs/input/android/os/InputConfig.aidl
const INPUT_CONFIG_FLAGS: ReadonlyArray<readonly [number, string]> = [
  [1 << 0, 'NO_INPUT_CHANNEL'],
  [1 << 1, 'NOT_VISIBLE'],
  [1 << 2, 'NOT_FOCUSABLE'],
  [1 << 3, 'NOT_TOUCHABLE'],
  [1 << 4, 'PREVENT_SPLITTING'],
  [1 << 5, 'DUPLICATE_TOUCH_TO_WALLPAPER'],
  [1 << 6, 'IS_WALLPAPER'],
  [1 << 7, 'PAUSE_DISPATCHING'],
  [1 << 8, 'TRUSTED_OVERLAY'],
  [1 << 9, 'WATCH_OUTSIDE_TOUCH'],
  [1 << 10, 'SLIPPERY'],
  [1 << 11, 'DISABLE_USER_ACTIVITY'],
  [1 << 12, 'DROP_INPUT'],
  [1 << 13, 'DROP_INPUT_IF_OBSCURED'],
  [1 << 14, 'SPY'],
  [1 << 15, 'INTERCEPTS_STYLUS'],
  [1 << 16, 'CLONE'],
  [1 << 17, 'GLOBAL_STYLUS_BLOCKS_TOUCH'],
];

// Flag names, with unknown bits as one hex value at the end.
export function decodeInputConfig(config: number): string[] {
  const names: string[] = [];
  let rest = config;
  for (const [bit, name] of INPUT_CONFIG_FLAGS) {
    if ((config & bit) !== 0) {
      names.push(name);
      rest &= ~bit;
    }
  }
  if (rest !== 0) names.push(`0x${(rest >>> 0).toString(16)}`);
  return names;
}

// The bounding box of `r` mapped by `t`, or undefined when empty.
export function transformRect(r: XywhRect, t?: Affine): SfRect | undefined {
  if (!(r.w > 0 && r.h > 0)) return undefined;
  const corners: ReadonlyArray<readonly [number, number]> = [
    [r.x, r.y],
    [r.x + r.w, r.y],
    [r.x, r.y + r.h],
    [r.x + r.w, r.y + r.h],
  ];
  const pts = corners.map(([x, y]) =>
    t === undefined
      ? [x, y]
      : [t.dsdx * x + t.dtdx * y + t.tx, t.dtdy * x + t.dsdy * y + t.ty],
  );
  const xs = pts.map((p) => p[0]);
  const ys = pts.map((p) => p[1]);
  return {
    left: Math.min(...xs),
    top: Math.min(...ys),
    right: Math.max(...xs),
    bottom: Math.max(...ys),
  };
}

export function buildSfInput(row: SfInputRow): SfInput {
  const touchable: SfRect[] = [];
  for (const r of row.region) {
    const t = transformRect(r, row.transform);
    if (t !== undefined) touchable.push(t);
  }
  return {
    frame: transformRect(row.frame, row.transform),
    touchable,
    depth: row.depth,
    isVisible: row.isVisible,
    isSpy: row.isSpy,
    focusable: row.focusable,
    layoutParamsType: row.layoutParamsType,
    config: row.config,
  };
}

// The layer of a WM window that takes its input, if any: usually the
// buffer layer.
export function windowInputLayer(layers: SfWindowLayers): SfLayer | undefined {
  return [layers.buffer, layers.container, layers.leash].find(
    (l) => l?.input !== undefined,
  );
}

export interface InputCanvas {
  // Copies of the tree nodes taking input with their input frame as bounds,
  // bottom of the input z-order first.
  readonly nodes: UiHierarchyNode[];
  // The touchable region of each node, by node id.
  readonly touchable: ReadonlyMap<string, ReadonlyArray<SfRect>>;
}

// The input windows among `layers`, drawn as the tree node `nodeFor` maps
// their layer to (layers without one are left out). When several layers
// map to one node, the top one wins.
export function inputCanvas(
  layers: ReadonlyArray<SfLayer>,
  nodeFor: (layer: SfLayer) => UiHierarchyNode | undefined,
): InputCanvas {
  interface Item {
    readonly node: UiHierarchyNode;
    readonly input: SfInput;
    readonly frame: SfRect;
  }
  const top = new Map<string, Item>();
  for (const layer of layers) {
    const input = layer.input;
    const frame = input?.frame;
    if (input === undefined || frame === undefined) continue;
    const node = nodeFor(layer);
    if (node === undefined) continue;
    const seen = top.get(node.nodeId);
    if (seen === undefined || seen.input.depth < input.depth) {
      top.set(node.nodeId, {node, input, frame});
    }
  }
  const items = [...top.values()].sort((a, b) => a.input.depth - b.input.depth);
  const nodes: UiHierarchyNode[] = [];
  const touchable = new Map<string, ReadonlyArray<SfRect>>();
  for (const {node, input, frame: f} of items) {
    nodes.push({
      ...node,
      boundsLeft: f.left,
      boundsTop: f.top,
      boundsRight: f.right,
      boundsBottom: f.bottom,
      isVisible: input.isVisible,
    });
    touchable.set(node.nodeId, input.touchable);
  }
  return {nodes, touchable};
}

// The Input properties of a layer. `withType` adds the window type, for
// layers shown without their WM window.
export function inputRows(input: SfInput, withType: boolean): PropRow[] {
  const b = new PropRowsBuilder();
  if (input.frame !== undefined) {
    b.text('Frame', formatBounds(input.frame), {mono: true});
  }
  if (input.touchable.length === 0) {
    b.text('Touchable', 'none');
  } else {
    b.list(
      'Touchable',
      input.touchable.map((r) => formatBounds(r)),
    );
  }
  b.text('Visible', String(input.isVisible));
  b.text('Focusable', String(input.focusable));
  if (withType && input.layoutParamsType !== 0) {
    b.text('Type', windowTypeToString(input.layoutParamsType), {mono: true});
  }
  if (input.config !== 0) {
    b.text('Config', decodeInputConfig(input.config).join(' | '), {
      title: `0x${(input.config >>> 0).toString(16)}`,
    });
  }
  return b.build();
}

// The input windows of SF snapshot `snapshot`, by layer id. Empty with
// trace processors without the Winscope rect modules.
export async function querySfInput(
  engine: Engine,
  snapshot: SfSnapshot,
): Promise<Map<number, SfInput>> {
  try {
    await engine.query(`
      INCLUDE PERFETTO MODULE android.winscope.rect;
      INCLUDE PERFETTO MODULE android.winscope.surfaceflinger;
    `);
  } catch {
    // Older trace processor without the modules: no input data.
    return new Map();
  }
  const res = await engine.query(`
    SELECT
      l.layer_id,
      tr.depth,
      tr.is_visible,
      tr.is_spy,
      r.x, r.y, r.w, r.h,
      t.dsdx, t.dtdx, t.tx, t.dtdy, t.dsdy, t.ty,
      fr.x AS fx, fr.y AS fy, fr.w AS fw, fr.h AS fh,
      extract_arg(l.arg_set_id, 'input_window_info.focusable') AS focusable,
      extract_arg(l.arg_set_id, 'input_window_info.layout_params_type')
        AS type,
      extract_arg(l.arg_set_id, 'input_window_info.input_config') AS config
    FROM surfaceflinger_layer l
    JOIN android_winscope_trace_rect tr ON tr.id = l.input_rect_id
    JOIN android_winscope_rect r ON r.id = tr.rect_id
    LEFT JOIN android_winscope_transform t ON t.id = tr.transform_id
    LEFT JOIN android_winscope_fill_region f ON f.trace_rect_id = tr.id
    LEFT JOIN android_winscope_rect fr ON fr.id = f.rect_id
    WHERE l.snapshot_id = ${snapshot.id}
    ORDER BY tr.depth, f.id;
  `);
  const rows = new Map<number, SfInputRow & {region: XywhRect[]}>();
  const it = res.iter({
    layer_id: NUM,
    depth: NUM_NULL,
    is_visible: NUM_NULL,
    is_spy: NUM_NULL,
    x: NUM,
    y: NUM,
    w: NUM,
    h: NUM,
    dsdx: NUM_NULL,
    dtdx: NUM_NULL,
    tx: NUM_NULL,
    dtdy: NUM_NULL,
    dsdy: NUM_NULL,
    ty: NUM_NULL,
    fx: NUM_NULL,
    fy: NUM_NULL,
    fw: NUM_NULL,
    fh: NUM_NULL,
    focusable: NUM_NULL,
    type: NUM_NULL,
    config: NUM_NULL,
  });
  for (; it.valid(); it.next()) {
    let row = rows.get(it.layer_id);
    if (row === undefined) {
      row = {
        layerId: it.layer_id,
        frame: {x: it.x, y: it.y, w: it.w, h: it.h},
        transform:
          it.dsdx !== null &&
          it.dtdx !== null &&
          it.tx !== null &&
          it.dtdy !== null &&
          it.dsdy !== null &&
          it.ty !== null
            ? {
                dsdx: it.dsdx,
                dtdx: it.dtdx,
                tx: it.tx,
                dtdy: it.dtdy,
                dsdy: it.dsdy,
                ty: it.ty,
              }
            : undefined,
        region: [],
        depth: it.depth ?? 0,
        isVisible: it.is_visible === 1,
        isSpy: it.is_spy === 1,
        focusable: it.focusable === 1,
        layoutParamsType: it.type ?? 0,
        config: it.config ?? 0,
      };
      rows.set(it.layer_id, row);
    }
    if (it.fx !== null && it.fy !== null && it.fw !== null && it.fh !== null) {
      row.region.push({x: it.fx, y: it.fy, w: it.fw, h: it.fh});
    }
  }
  const out = new Map<number, SfInput>();
  for (const [id, row] of rows) out.set(id, buildSfInput(row));
  return out;
}
