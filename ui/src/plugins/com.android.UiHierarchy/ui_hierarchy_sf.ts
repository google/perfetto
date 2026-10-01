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

// SurfaceFlinger (android.surfaceflinger.layers) data for the Screen level:
// the layers of one SF snapshot, how they link to WM windows, and the
// curated subset of layer properties shown in the viewer. Visibility,
// occlusion and screen bounds are precomputed by trace processor (the same
// computations Winscope runs) and stored as layer args.

import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import type {UiHierarchyNode} from './ui_hierarchy_data';
import {querySfInput, type SfInput} from './ui_hierarchy_input';
import {
  displayName,
  formatBounds,
  LEASH_MARKER,
  formatNumber,
  type PropLink,
  type PropRow,
  PropRowsBuilder,
} from './ui_hierarchy_props';
import {SCREEN_KIND_LAYER} from './ui_hierarchy_wm';

// Layers created by system_server (WM containers, leashes) use this uid.
export const SYSTEM_UID = 1000;

// Sentinel layer id for "no layer" (parent, relative-z parent).
const NO_LAYER = -1;

export interface SfSnapshot {
  readonly id: number;
  readonly ts: bigint;
}

export interface SfRect {
  readonly left: number;
  readonly top: number;
  readonly right: number;
  readonly bottom: number;
}

export interface SfColor {
  readonly r: number;
  readonly g: number;
  readonly b: number;
  readonly a: number;
}

export interface SfBuffer {
  readonly width: number;
  readonly height: number;
  // Pixel format name, e.g. 'RGBA_8888'.
  readonly format: string;
}

// One layer of an SF snapshot. Optional fields are undefined when absent or
// set to a sentinel ("no crop", "infinite bounds", "unset color").
export interface SfLayer {
  readonly id: number;
  readonly name: string;
  readonly parentId?: number;
  readonly relativeOf?: number;
  readonly isVisible: boolean;
  // HwcCompositionType (see compositionLabel).
  readonly composition: number;
  readonly ownerUid?: number;
  readonly layerStack?: number;
  readonly z: number;
  readonly flags: number;
  readonly visibilityReasons: ReadonlyArray<string>;
  readonly occludedBy: ReadonlyArray<number>;
  readonly partiallyOccludedBy: ReadonlyArray<number>;
  readonly coveredBy: ReadonlyArray<number>;
  readonly screenBounds?: SfRect;
  readonly crop?: SfRect;
  readonly buffer?: SfBuffer;
  readonly color?: SfColor;
  readonly alpha: number;
  readonly cornerRadius: number;
  readonly shadowRadius: number;
  readonly blurRadius: number;
  readonly transformType: number;
  readonly frameNumber: number;
  // Set for layers that take input.
  readonly input?: SfInput;
}

// A layer row as queried: the table columns plus the curated args, keyed by
// the arg key (array args keep their index, e.g. 'covered_by[0]').
export interface SfLayerRow {
  readonly layerId: number;
  readonly name: string;
  readonly parent: number;
  readonly relativeOf: number;
  readonly isVisible: boolean;
  readonly composition: number;
  readonly args: ReadonlyMap<string, number | string>;
}

// Arg keys (flat, without array indices) read from each layer.
const LAYER_ARGS = [
  'owner_uid',
  'layer_stack',
  'z',
  'flags',
  'visibility_reason',
  'occluded_by',
  'partially_occluded_by',
  'covered_by',
  'screen_bounds.left',
  'screen_bounds.top',
  'screen_bounds.right',
  'screen_bounds.bottom',
  'crop.left',
  'crop.top',
  'crop.right',
  'crop.bottom',
  'active_buffer.width',
  'active_buffer.height',
  'active_buffer.format',
  'color.r',
  'color.g',
  'color.b',
  'color.a',
  'corner_radius',
  'corner_radii.tl',
  'corner_radii.tr',
  'corner_radii.bl',
  'corner_radii.br',
  'shadow_radius',
  'background_blur_radius',
  'transform.type',
  'curr_frame',
];

// android.graphics.PixelFormat / AHardwareBuffer formats.
const PIXEL_FORMATS: Record<number, string> = {
  1: 'RGBA_8888',
  2: 'RGBX_8888',
  3: 'RGB_888',
  4: 'RGB_565',
  5: 'BGRA_8888',
  0x16: 'RGBA_FP16',
  0x2b: 'RGBA_1010102',
  0x38: 'R_8',
};

export function pixelFormatName(format: number): string {
  return PIXEL_FORMATS[format] ?? `format ${format}`;
}

// layer_state_t flags (frameworks/native/libs/gui/include/gui/LayerState.h).
const LAYER_FLAGS: ReadonlyArray<readonly [number, string]> = [
  [0x01, 'HIDDEN'],
  [0x02, 'OPAQUE'],
  [0x40, 'SKIP_SCREENSHOT'],
  [0x80, 'SECURE'],
  [0x100, 'ENABLE_BACKPRESSURE'],
  [0x200, 'DISPLAY_DECORATION'],
  [0x400, 'IGNORE_DESTINATION_FRAME'],
  [0x800, 'REFRESH_RATE_INDICATOR'],
  [0x1000, 'CAN_OCCLUDE_PRESENTATION'],
  [0x2000, 'RECOVERABLE_FROM_BUFFER_STUFFING'],
];

// Flag names, with unknown bits as one hex value at the end.
export function decodeLayerFlags(flags: number): string[] {
  const names: string[] = [];
  let rest = flags;
  for (const [bit, name] of LAYER_FLAGS) {
    if ((flags & bit) !== 0) {
      names.push(name);
      rest &= ~bit;
    }
  }
  if (rest !== 0) names.push(`0x${(rest >>> 0).toString(16)}`);
  return names;
}

// ui::Transform type bits (frameworks/native/libs/ui/Transform.h), named
// as Winscope does.
const ROT_INVALID = 0x8000;
const ROT_90 = 0x400;
const FLIP_V = 0x200;
const FLIP_H = 0x100;
const TRANSFORM_KINDS: ReadonlyArray<readonly [number, string]> = [
  [0x4, 'SCALE'],
  [0x2, 'ROTATE'],
  [0x1, 'TRANSLATE'],
];

// Transform type names: the orientation, then the matrix kinds, with
// unknown bits as one hex value at the end.
export function decodeTransformType(type: number): string[] {
  const names: string[] = [];
  const orient = type & (ROT_INVALID | ROT_90 | FLIP_V | FLIP_H);
  const all = (bits: number) => (orient & bits) === bits;
  if (all(ROT_INVALID)) names.push('ROT_INVALID');
  else if (all(ROT_90 | FLIP_V | FLIP_H)) names.push('ROT_270');
  else if (all(FLIP_V | FLIP_H)) names.push('ROT_180');
  else {
    if (all(ROT_90)) names.push('ROT_90');
    if (all(FLIP_V)) names.push('FLIP_V');
    if (all(FLIP_H)) names.push('FLIP_H');
  }
  let rest = type & ~orient;
  for (const [bit, name] of TRANSFORM_KINDS) {
    if ((rest & bit) !== 0) {
      names.push(name);
      rest &= ~bit;
    }
  }
  if (rest !== 0) names.push(`0x${(rest >>> 0).toString(16)}`);
  return names;
}

// HwcCompositionType: DEVICE is composed by HWC, CLIENT by the GPU.
const COMPOSITION_LABELS: Record<number, string> = {
  1: 'GPU',
  2: 'HWC',
  3: 'SOLID_COLOR',
  4: 'CURSOR',
  5: 'SIDEBAND',
  6: 'DISPLAY_DECORATION',
};

// The composition chip label, or undefined when there is nothing to say
// (invisible layers are never composed, so UNSPECIFIED is noise there).
export function compositionLabel(layer: SfLayer): string | undefined {
  const label = COMPOSITION_LABELS[layer.composition];
  if (label !== undefined) return label;
  return layer.isVisible ? 'UNSPECIFIED' : undefined;
}

// SF reports unbounded layers with bounds symmetric around the origin
// (10x the largest display), e.g. (-22080, -20920) - (22080, 20920).
export function isInfiniteBounds(r: SfRect): boolean {
  return r.left < 0 && r.top < 0 && r.right === -r.left && r.bottom === -r.top;
}

export function buildSfLayer(row: SfLayerRow): SfLayer {
  const a = row.args;
  const num = (key: string): number | undefined => {
    const v = a.get(key);
    return typeof v === 'number' ? v : undefined;
  };
  const list = <T>(key: string, map: (v: number | string) => T): T[] => {
    const out: T[] = [];
    for (let i = 0; a.has(`${key}[${i}]`); i++) {
      out.push(map(a.get(`${key}[${i}]`) ?? ''));
    }
    return out;
  };
  const rect = (prefix: string): SfRect | undefined => {
    const left = num(`${prefix}.left`);
    const top = num(`${prefix}.top`);
    const right = num(`${prefix}.right`);
    const bottom = num(`${prefix}.bottom`);
    if ([left, top, right, bottom].every((v) => v === undefined)) {
      return undefined;
    }
    return {
      left: left ?? 0,
      top: top ?? 0,
      right: right ?? 0,
      bottom: bottom ?? 0,
    };
  };

  const bounds = rect('screen_bounds');
  // An unset crop is (0, 0) - (-1, -1); any empty crop crops nothing.
  const crop = rect('crop');
  const width = num('active_buffer.width') ?? 0;
  const height = num('active_buffer.height') ?? 0;
  const format = num('active_buffer.format');
  const color: SfColor = {
    r: num('color.r') ?? 0,
    g: num('color.g') ?? 0,
    b: num('color.b') ?? 0,
    a: num('color.a') ?? 1,
  };
  const radii = ['tl', 'tr', 'bl', 'br'].map(
    (c) => num(`corner_radii.${c}`) ?? 0,
  );
  return {
    id: row.layerId,
    name: row.name,
    parentId: row.parent !== NO_LAYER ? row.parent : undefined,
    relativeOf: row.relativeOf !== NO_LAYER ? row.relativeOf : undefined,
    isVisible: row.isVisible,
    composition: row.composition,
    ownerUid: num('owner_uid'),
    layerStack: num('layer_stack'),
    z: num('z') ?? 0,
    flags: num('flags') ?? 0,
    visibilityReasons: list('visibility_reason', String),
    occludedBy: list('occluded_by', Number),
    partiallyOccludedBy: list('partially_occluded_by', Number),
    coveredBy: list('covered_by', Number),
    screenBounds:
      bounds !== undefined && !isInfiniteBounds(bounds) ? bounds : undefined,
    crop:
      crop !== undefined && crop.right > crop.left && crop.bottom > crop.top
        ? crop
        : undefined,
    buffer:
      width > 0 && height > 0 && format !== undefined
        ? {width, height, format: pixelFormatName(format)}
        : undefined,
    // A channel of -1 means "no color set".
    color: [color.r, color.g, color.b, color.a].some((v) => v < 0)
      ? undefined
      : color,
    alpha: color.a,
    cornerRadius: Math.max(num('corner_radius') ?? 0, ...radii),
    shadowRadius: num('shadow_radius') ?? 0,
    blurRadius: num('background_blur_radius') ?? 0,
    transformType: num('transform.type') ?? 0,
    frameNumber: num('curr_frame') ?? 0,
  };
}

// The layers of one snapshot with parent/child links. Children are in z
// order, bottom first (like WM children).
export class SfLayerIndex {
  readonly byId = new Map<number, SfLayer>();
  readonly roots: SfLayer[] = [];
  private readonly children = new Map<number, SfLayer[]>();

  constructor(layers: ReadonlyArray<SfLayer>) {
    for (const l of layers) this.byId.set(l.id, l);
    for (const l of layers) {
      const parent = l.parentId;
      if (parent === undefined || !this.byId.has(parent)) {
        this.roots.push(l);
        continue;
      }
      const arr = this.children.get(parent);
      if (arr) arr.push(l);
      else this.children.set(parent, [l]);
    }
    for (const arr of this.children.values()) arr.sort((a, b) => a.z - b.z);
  }

  get size(): number {
    return this.byId.size;
  }

  childrenOf(id: number): ReadonlyArray<SfLayer> {
    return this.children.get(id) ?? [];
  }

  // `root` and its descendants in depth-first (paint) order, skipping the
  // subtrees rooted at `exclude`.
  subtree(root: SfLayer, exclude?: ReadonlySet<number>): SfLayer[] {
    const out: SfLayer[] = [];
    const seen = new Set<number>();
    const visit = (l: SfLayer) => {
      if (seen.has(l.id)) return; // Guards against cycles.
      seen.add(l.id);
      out.push(l);
      for (const c of this.childrenOf(l.id)) {
        if (exclude?.has(c.id) !== true) visit(c);
      }
    };
    visit(root);
    return out;
  }

  // The root layers drawn on `layerStack` (all roots but offscreen ones
  // when unknown).
  rootsOnStack(layerStack: number | undefined): SfLayer[] {
    return this.roots.filter((r) =>
      layerStack === undefined
        ? r.layerStack !== undefined
        : r.layerStack === layerStack,
    );
  }

  // The root layers not drawn on any display (e.g. "Offscreen Root", which
  // holds the buffer layers of hidden windows).
  get offscreenRoots(): SfLayer[] {
    return this.roots.filter((r) => r.layerStack === undefined);
  }

  // The root layers drawn on the display whose root layer is
  // `displayLayerId`: all on-screen roots when the id is unknown, none when
  // the layer is not in this snapshot (e.g. a virtual display SF has not
  // composited yet).
  displayRoots(displayLayerId: number | undefined): SfLayer[] {
    if (displayLayerId === undefined) return this.rootsOnStack(undefined);
    const display = this.byId.get(displayLayerId);
    return display?.layerStack !== undefined
      ? this.rootsOnStack(display.layerStack)
      : [];
  }

  // `ids` with their ancestors and descendants.
  withContext(ids: Iterable<number>): Set<number> {
    const out = new Set<number>();
    for (const id of ids) {
      const l = this.byId.get(id);
      if (l === undefined) continue;
      for (const d of this.subtree(l)) out.add(d.id);
      for (
        let p =
          l.parentId !== undefined ? this.byId.get(l.parentId) : undefined;
        p !== undefined && !out.has(p.id);
        p = p.parentId !== undefined ? this.byId.get(p.parentId) : undefined
      ) {
        out.add(p.id);
      }
    }
    return out;
  }

  // The number of distinct layer stacks among the root layers.
  get layerStackCount(): number {
    return new Set(
      this.roots.map((r) => r.layerStack).filter((s) => s !== undefined),
    ).size;
  }
}

// The SF layers drawing a WM window: its container (the WindowState's
// surface), the buffer layer the app draws into (`VRI-<title>#id`, or a
// BBQ wrapper) and the animation leash the container is reparented to while
// animating, if any.
export interface SfWindowLayers {
  readonly container?: SfLayer;
  readonly buffer?: SfLayer;
  readonly leash?: SfLayer;
}

// Links a WM window (by its surface layer id and title) to SF layers.
// `otherContainers` are the container layers of the other WM windows: child
// windows are SF descendants of their parent window and must not be taken
// for its buffer.
export function linkWindowLayers(
  index: SfLayerIndex,
  layerId: number | undefined,
  title: string,
  otherContainers: ReadonlySet<number>,
): SfWindowLayers {
  const container = layerId !== undefined ? index.byId.get(layerId) : undefined;
  let buffer: SfLayer | undefined;
  if (container !== undefined) {
    const own = index.subtree(container, otherContainers).slice(1);
    buffer =
      own.find((l) => l.isVisible && l.buffer !== undefined) ??
      own.find((l) => l.buffer !== undefined) ??
      own.find((l) => l.name.startsWith('VRI-'));
  }
  // Hidden windows can have their buffer layer reparented offscreen; it
  // keeps the `VRI-<title>#id` name.
  if (buffer === undefined) {
    const prefix = `VRI-${title}#`;
    for (const l of index.byId.values()) {
      if (l.name.startsWith(prefix)) {
        buffer = l;
        break;
      }
    }
  }
  let leash: SfLayer | undefined;
  const parent =
    container?.parentId !== undefined
      ? index.byId.get(container.parentId)
      : undefined;
  if (parent !== undefined && parent.name.includes(LEASH_MARKER)) {
    leash = parent;
  }
  return {container, buffer, leash};
}

// The layer that stands for a window in the SurfaceFlinger tree and
// properties: the buffer it draws into, else its container.
export function windowLayer(layers: SfWindowLayers): SfLayer | undefined {
  return layers.buffer ?? layers.container;
}

// The WM window (node id) drawn by `layerId`: the nearest window whose
// container, buffer or leash is the layer or one of its ancestors.
export function windowOfLayer(
  index: SfLayerIndex,
  links: ReadonlyMap<string, SfWindowLink>,
  layerId: number,
): string | undefined {
  const byLayer = new Map<number, string>();
  for (const [nodeId, {layers}] of links) {
    for (const l of [layers.leash, layers.container, layers.buffer]) {
      if (l !== undefined) byLayer.set(l.id, nodeId);
    }
  }
  const seen = new Set<number>();
  for (
    let l = index.byId.get(layerId);
    l !== undefined && !seen.has(l.id);
    l = l.parentId !== undefined ? index.byId.get(l.parentId) : undefined
  ) {
    const nodeId = byLayer.get(l.id);
    if (nodeId !== undefined) return nodeId;
    seen.add(l.id);
  }
  return undefined;
}

// The uid of the app drawing a window: the owner of its buffer layer, or of
// any layer under its container, other than system_server.
export function windowAppUid(
  index: SfLayerIndex,
  layers: SfWindowLayers,
  otherContainers: ReadonlySet<number>,
): number | undefined {
  const isApp = (uid: number | undefined): uid is number =>
    uid !== undefined && uid !== SYSTEM_UID && uid !== 0;
  if (isApp(layers.buffer?.ownerUid)) return layers.buffer.ownerUid;
  if (layers.container === undefined) return undefined;
  return index
    .subtree(layers.container, otherContainers)
    .map((l) => l.ownerUid)
    .find(isApp);
}

export interface SfWindowLink {
  readonly layers: SfWindowLayers;
  readonly appUid?: number;
}

// Links WM windows to the layers of `index`, by window node id.
export function linkWmWindows(
  windows: ReadonlyArray<UiHierarchyNode>,
  index: SfLayerIndex,
): Map<string, SfWindowLink> {
  const containers = new Set<number>();
  for (const w of windows) {
    if (w.wm?.layerId !== undefined) containers.add(w.wm.layerId);
  }
  const out = new Map<string, SfWindowLink>();
  for (const w of windows) {
    const id = w.wm?.layerId;
    const others = new Set([...containers].filter((c) => c !== id));
    const layers = linkWindowLayers(index, id, w.name, others);
    out.set(w.nodeId, {layers, appUid: windowAppUid(index, layers, others)});
  }
  return out;
}

// The short "Name#id" label of a layer. SF appends the id to every layer
// name but the offscreen root.
export function layerLabel(layer: SfLayer): string {
  return displayName(layer.name);
}

// Visibility reasons with layer ids replaced by layer names.
export function describeVisibilityReasons(
  layer: SfLayer,
  index: SfLayerIndex,
): string[] {
  return layer.visibilityReasons.map((r) => {
    const m = /^hidden by parent (\d+)$/.exec(r);
    if (m === null) return r;
    const parent = index.byId.get(Number(m[1]));
    return parent !== undefined ? `hidden by parent ${layerLabel(parent)}` : r;
  });
}

function layerLink(index: SfLayerIndex, id: number): PropLink {
  const l = index.byId.get(id);
  const target = {kind: 'layer', layerId: id} as const;
  return l !== undefined
    ? {text: layerLabel(l), title: l.name, target}
    : {text: `#${id}`, title: `Layer ${id} (not in this snapshot)`, target};
}

// The curated SurfaceFlinger properties of `layer` (Winscope's curated
// set), skipping defaults and sentinels. For the layer of a window,
// `window` holds the window's layers, which lead the rows as links.
export function sfLayerRows(
  index: SfLayerIndex,
  layer: SfLayer,
  window?: SfWindowLayers,
): PropRow[] {
  const b = new PropRowsBuilder();
  const links = (label: string, ids: ReadonlyArray<number>) =>
    b.links(
      label,
      ids.map((id) => layerLink(index, id)),
    );
  const f = formatNumber;

  if (window !== undefined) {
    links('Layer', [layer.id]);
    if (window.container !== undefined && window.container.id !== layer.id) {
      links('Container', [window.container.id]);
    }
    if (window.leash !== undefined) links('Leash', [window.leash.id]);
  }

  b.text('Visible', String(layer.isVisible));
  if (!layer.isVisible) {
    b.list('Invisible due to', describeVisibilityReasons(layer, index));
  }
  links('Occluded by', layer.occludedBy);
  links('Partially occluded by', layer.partiallyOccludedBy);
  links('Covered by', layer.coveredBy);
  b.chip('Composition', compositionLabel(layer));
  if (layer.flags !== 0) {
    b.text('Flags', decodeLayerFlags(layer.flags).join(' | '), {
      title: `0x${(layer.flags >>> 0).toString(16)}`,
    });
  }
  const buf = layer.buffer;
  if (buf !== undefined) {
    b.text('Buffer', `${buf.width}\u00d7${buf.height} \u00b7 ${buf.format}`);
  }
  if (layer.screenBounds !== undefined) {
    b.text('Screen bounds', formatBounds(layer.screenBounds), {mono: true});
  }
  if (layer.crop !== undefined) {
    b.text('Crop', formatBounds(layer.crop), {mono: true});
  }
  b.text('Z', `${layer.z}`);
  if (layer.relativeOf !== undefined) links('Relative to', [layer.relativeOf]);
  if (layer.alpha !== 1) b.text('Opacity', f(layer.alpha));
  // Only fills (layers without a buffer) draw their color.
  const c = layer.color;
  if (c !== undefined && buf === undefined && layer.isVisible) {
    b.text('Color', `rgba(${f(c.r)}, ${f(c.g)}, ${f(c.b)}, ${f(c.a)})`);
  }
  if (layer.cornerRadius > 0) {
    b.text('Corner radius', `${f(layer.cornerRadius)} px`);
  }
  if (layer.shadowRadius > 0) {
    b.text('Shadow radius', `${f(layer.shadowRadius)} px`);
  }
  if (layer.blurRadius > 0) {
    b.text('Background blur', `${f(layer.blurRadius)} px`);
  }
  if (layer.layerStack !== undefined && index.layerStackCount > 1) {
    b.text('Layer stack', `${layer.layerStack}`);
  }
  if (layer.transformType !== 0) {
    b.text('Transform', decodeTransformType(layer.transformType).join(' | '), {
      title: `0x${(layer.transformType >>> 0).toString(16)}`,
    });
  }
  if (layer.frameNumber !== 0) b.text('Frame', `${layer.frameNumber}`);
  return b.build();
}

export interface SfCompositionCounts {
  readonly visible: number;
  readonly hwc: number;
  readonly gpu: number;
}

// Visible layers and how they were composed.
export function countCompositions(
  layers: ReadonlyArray<SfLayer>,
): SfCompositionCounts {
  let visible = 0;
  let hwc = 0;
  let gpu = 0;
  for (const l of layers) {
    if (!l.isVisible) continue;
    visible++;
    const label = compositionLabel(l);
    if (label === 'GPU') gpu++;
    else if (label !== undefined && label !== 'UNSPECIFIED') hwc++;
  }
  return {visible, hwc, gpu};
}

export function sfNodeId(layerId: number): string {
  return `sf:${layerId}`;
}

// Tree nodes for the SurfaceFlinger mode: `roots` and their descendants,
// in paint order (bottom first), restricted to `keep` if given (which must
// hold the ancestors of its layers).
export function buildSfTreeNodes(
  index: SfLayerIndex,
  roots: ReadonlyArray<SfLayer>,
  ts: bigint,
  keep?: ReadonlySet<number>,
): UiHierarchyNode[] {
  const out: UiHierarchyNode[] = [];
  const childCount = new Map<number, number>();
  for (const root of roots) {
    for (const l of index.subtree(root)) {
      if (keep !== undefined && !keep.has(l.id)) continue;
      const parent =
        l.id !== root.id && l.parentId !== undefined ? l.parentId : undefined;
      const childIndex = childCount.get(parent ?? NO_LAYER) ?? 0;
      childCount.set(parent ?? NO_LAYER, childIndex + 1);
      const b = l.screenBounds;
      out.push({
        rowId: out.length,
        ts,
        dur: 0n,
        windowId: 0,
        nodeId: sfNodeId(l.id),
        parentNodeId: parent !== undefined ? sfNodeId(parent) : undefined,
        childIndex,
        kind: SCREEN_KIND_LAYER,
        kindName: 'Layer',
        name: l.name,
        boundsLeft: b?.left ?? 0,
        boundsTop: b?.top ?? 0,
        boundsRight: b?.right ?? 0,
        boundsBottom: b?.bottom ?? 0,
        alpha: l.alpha,
        flags: BigInt(l.flags >>> 0),
        isVisible: l.isVisible,
        isClickable: false,
        isFocused: false,
        isTextRedacted: false,
        sfLayerId: l.id,
      });
    }
  }
  return out;
}

// The last snapshot at or before `ts` (state snapshots apply until the next
// one), else the first.
export function indexAtOrBefore(
  items: ReadonlyArray<{readonly ts: bigint}>,
  ts: bigint,
): number {
  let best = 0;
  for (let i = 0; i < items.length && items[i].ts <= ts; i++) best = i;
  return best;
}

export async function querySfSnapshots(engine: Engine): Promise<SfSnapshot[]> {
  try {
    const res = await engine.query(`
      SELECT id, ts FROM surfaceflinger_layers_snapshot ORDER BY ts;
    `);
    const out: SfSnapshot[] = [];
    for (const it = res.iter({id: NUM, ts: LONG}); it.valid(); it.next()) {
      out.push({id: it.id, ts: it.ts});
    }
    return out;
  } catch {
    // Trace processors without SurfaceFlinger tables.
    return [];
  }
}

export async function querySfLayers(
  engine: Engine,
  snapshot: SfSnapshot,
): Promise<SfLayer[]> {
  const keys = LAYER_ARGS.map((k) => `'${k}'`).join(', ');
  const inputs = querySfInput(engine, snapshot);
  const res = await engine.query(`
    SELECT
      l.id,
      l.layer_id,
      l.layer_name,
      l.parent,
      l.z_order_relative_of,
      l.is_visible,
      l.hwc_composition_type,
      a.key,
      COALESCE(a.int_value, a.real_value) AS num,
      a.string_value AS str
    FROM surfaceflinger_layer l
    LEFT JOIN args a ON a.arg_set_id = l.arg_set_id AND a.flat_key IN (${keys})
    WHERE l.snapshot_id = ${snapshot.id}
    ORDER BY l.id;
  `);
  const rows = new Map<
    number,
    Omit<SfLayerRow, 'args'> & {args: Map<string, number | string>}
  >();
  const it = res.iter({
    id: NUM,
    layer_id: NUM,
    layer_name: STR_NULL,
    parent: NUM_NULL,
    z_order_relative_of: NUM_NULL,
    is_visible: NUM,
    hwc_composition_type: NUM_NULL,
    key: STR_NULL,
    num: NUM_NULL,
    str: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    let row = rows.get(it.id);
    if (row === undefined) {
      row = {
        layerId: it.layer_id,
        name: it.layer_name ?? `Layer ${it.layer_id}`,
        parent: it.parent ?? NO_LAYER,
        relativeOf: it.z_order_relative_of ?? NO_LAYER,
        isVisible: it.is_visible !== 0,
        composition: it.hwc_composition_type ?? 0,
        args: new Map(),
      };
      rows.set(it.id, row);
    }
    if (it.key === null) continue;
    const value = it.num ?? it.str;
    if (value !== null) row.args.set(it.key, value);
  }
  const input = await inputs;
  return [...rows.values()].map((row) => {
    const layer = buildSfLayer(row);
    const i = input.get(layer.id);
    return i !== undefined ? {...layer, input: i} : layer;
  });
}
