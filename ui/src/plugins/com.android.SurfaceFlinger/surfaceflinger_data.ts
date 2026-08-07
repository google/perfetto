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

// Data layer for the SurfaceFlinger viewer: typed queries over the
// trace_processor tables (snapshots, layers, rects, transforms, displays) plus
// per-layer proto args, and the derivation of the "curated" property summary.

import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export interface SfDisplay {
  displayId: string; // int64 as string (can exceed 2^53)
  displayName: string;
  isVirtual: boolean; // e.g. the video-encoder display created during recording
  // Layer-stack/group id; layers with this group belong to this display.
  // Undefined if the display has no trace rect (so it is not aliased onto the
  // real group 0).
  group: number | undefined;
}

export interface SfTransform {
  dsdx: number;
  dtdx: number;
  tx: number;
  dtdy: number;
  dsdy: number;
  ty: number;
}

// A display id is an int64 rendered as a base-10 string (it can exceed 2^53).
// It comes from the trace, never user input, but validate before splicing into
// SQL so a malformed value fails loudly instead of producing a syntax error.
export function sqlInt(value: string): string {
  if (!/^-?\d+$/.test(value)) {
    throw new Error(`Expected an integer literal, got: ${value}`);
  }
  return value;
}

export interface SfLayer {
  rowId: number; // __intrinsic_surfaceflinger_layer.id (for arg lookup)
  layerId: number;
  name: string;
  isVisible: boolean;
  parent: number; // parent layer id, -1 if none
  zOrderRelativeOf: number; // -1 if none
  isHiddenByPolicy: boolean;
  isMissingZParent: boolean;
  hwcCompositionType: number;
  argSetId: number | undefined;
  // String-pool id of the raw LayerProto. Identical protos share one id, so
  // comparing it between snapshots is an exact "did anything change" diff.
  base64ProtoId: number | undefined;
  drawDepth: number | undefined; // painter order from the computed rect
  opacity: number | undefined;
  isSpy: boolean;
  // Layer-stack/group id of this layer's rect — identifies which display it is
  // composited on (matches SfDisplay.group). Undefined for layers without a rect.
  groupId: number | undefined;
  // Screen-space rect (already transformed bounds) if the layer has one.
  rect?: {x: number; y: number; w: number; h: number};
  transform: SfTransform;
}

// One per (display, snapshot): the timeline anchor for the track.
export interface SfSnapshot {
  snapshotId: number;
  ts: bigint;
}

// Distinct displays present in the trace.
export async function queryDisplays(engine: Engine): Promise<SfDisplay[]> {
  const res = await engine.query(`
    SELECT
      d.display_id AS id,
      COALESCE(MAX(d.display_name), 'Display') AS name,
      MAX(IFNULL(d.is_virtual, 0)) AS virt,
      MAX(tr.group_id) AS grp
    FROM __intrinsic_surfaceflinger_display d
    LEFT JOIN __intrinsic_winscope_trace_rect tr ON tr.id = d.trace_rect_id
    GROUP BY d.display_id
    ORDER BY d.display_id
  `);
  const out: SfDisplay[] = [];
  const it = res.iter({id: LONG, name: STR, virt: NUM, grp: NUM_NULL});
  for (; it.valid(); it.next()) {
    out.push({
      displayId: it.id.toString(),
      displayName: it.name,
      isVirtual: it.virt !== 0,
      group: it.grp ?? undefined,
    });
  }
  return out;
}

// Snapshots that contain the given display, in time order. Each snapshot is a
// frame of layer state; the track shows one slice per snapshot.
export async function querySnapshots(
  engine: Engine,
  displayId: string,
): Promise<SfSnapshot[]> {
  const res = await engine.query(`
    SELECT s.id AS sid, s.ts AS ts
    FROM __intrinsic_surfaceflinger_layers_snapshot s
    WHERE s.id IN (
      SELECT snapshot_id FROM __intrinsic_surfaceflinger_display
      WHERE display_id = ${sqlInt(displayId)}
    )
    ORDER BY s.ts
  `);
  const out: SfSnapshot[] = [];
  const it = res.iter({sid: NUM, ts: LONG});
  for (; it.valid(); it.next()) out.push({snapshotId: it.sid, ts: it.ts});
  return out;
}

// All layers of a snapshot, with their computed screen rect, transform and the
// flags needed for the hierarchy chips.
export async function queryLayers(
  engine: Engine,
  snapshotId: number,
): Promise<SfLayer[]> {
  const res = await engine.query(`
    SELECT
      l.id AS rowId,
      IFNULL(l.layer_id, -1) AS layerId,
      IFNULL(l.layer_name, '<no name>') AS name,
      l.is_visible AS isVisible,
      IFNULL(l.parent, -1) AS parent,
      IFNULL(l.z_order_relative_of, -1) AS zrel,
      l.is_hidden_by_policy AS hidden,
      l.is_missing_z_parent AS missingZ,
      IFNULL(l.hwc_composition_type, 0) AS hwc,
      l.arg_set_id AS argSetId,
      l.base64_proto_id AS base64,
      tr.depth AS depth,
      tr.opacity AS opacity,
      tr.group_id AS grp,
      IFNULL(tr.is_spy, 0) AS isSpy,
      r.x AS x, r.y AS y, r.w AS w, r.h AS h,
      t.dsdx AS dsdx, t.dtdx AS dtdx, t.tx AS tx,
      t.dtdy AS dtdy, t.dsdy AS dsdy, t.ty AS ty
    FROM __intrinsic_surfaceflinger_layer l
    LEFT JOIN __intrinsic_winscope_trace_rect tr ON tr.id = l.layer_rect_id
    LEFT JOIN __intrinsic_winscope_rect r ON r.id = tr.rect_id
    LEFT JOIN __intrinsic_winscope_transform t ON t.id = tr.transform_id
    WHERE l.snapshot_id = ${snapshotId}
  `);
  const out: SfLayer[] = [];
  const it = res.iter({
    rowId: NUM,
    layerId: LONG,
    name: STR,
    isVisible: NUM,
    parent: LONG,
    zrel: LONG,
    hidden: NUM,
    missingZ: NUM,
    hwc: LONG,
    argSetId: NUM_NULL,
    base64: NUM_NULL,
    depth: NUM_NULL,
    opacity: NUM_NULL,
    grp: NUM_NULL,
    isSpy: NUM,
    x: NUM_NULL,
    y: NUM_NULL,
    w: NUM_NULL,
    h: NUM_NULL,
    dsdx: NUM_NULL,
    dtdx: NUM_NULL,
    tx: NUM_NULL,
    dtdy: NUM_NULL,
    dsdy: NUM_NULL,
    ty: NUM_NULL,
  });
  for (; it.valid(); it.next()) {
    const hasRect = it.w !== null && it.h !== null;
    out.push({
      rowId: it.rowId,
      layerId: Number(it.layerId),
      name: it.name,
      isVisible: it.isVisible !== 0,
      parent: Number(it.parent),
      zOrderRelativeOf: Number(it.zrel),
      isHiddenByPolicy: it.hidden !== 0,
      isMissingZParent: it.missingZ !== 0,
      hwcCompositionType: Number(it.hwc),
      argSetId: it.argSetId ?? undefined,
      base64ProtoId: it.base64 ?? undefined,
      drawDepth: it.depth ?? undefined,
      opacity: it.opacity ?? undefined,
      groupId: it.grp ?? undefined,
      isSpy: it.isSpy !== 0,
      rect: hasRect
        ? {x: it.x ?? 0, y: it.y ?? 0, w: it.w ?? 0, h: it.h ?? 0}
        : undefined,
      transform: {
        dsdx: it.dsdx ?? 1,
        dtdx: it.dtdx ?? 0,
        tx: it.tx ?? 0,
        dtdy: it.dtdy ?? 0,
        dsdy: it.dsdy ?? 1,
        ty: it.ty ?? 0,
      },
    });
  }
  return out;
}

// A flattened proto property: the arg `key` (with array indices) and its value.
export interface SfArg {
  key: string; // e.g. "color.a", "transform.dsdx", "occluded_by[0]"
  flatKey: string; // without array indices
  value: string | number | bigint;
  type: 'int' | 'real' | 'string' | 'bool' | 'null';
}

// All proto args for a layer's arg_set_id (the full property set).
export async function queryArgs(
  engine: Engine,
  argSetId: number,
): Promise<SfArg[]> {
  const res = await engine.query(`
    SELECT key, flat_key AS flatKey, int_value AS i, string_value AS s,
           real_value AS r, value_type AS vt
    FROM args WHERE arg_set_id = ${argSetId}
    ORDER BY key
  `);
  const out: SfArg[] = [];
  const it = res.iter({
    key: STR,
    flatKey: STR,
    i: LONG_NULL,
    s: STR_NULL,
    r: NUM_NULL,
    vt: STR,
  });
  for (; it.valid(); it.next()) {
    let value: string | number | bigint = '';
    let type: SfArg['type'] = 'null';
    if (it.vt === 'string' && it.s !== null) {
      value = it.s;
      type = 'string';
    } else if (it.vt === 'bool') {
      value = it.i !== null && it.i !== 0n ? 'true' : 'false';
      type = 'bool';
    } else if (it.vt === 'real' && it.r !== null) {
      value = it.r;
      type = 'real';
    } else if (it.i !== null) {
      value = it.i;
      type = 'int';
    } else if (it.s !== null) {
      value = it.s;
      type = 'string';
    }
    out.push({key: it.key, flatKey: it.flatKey, value, type});
  }
  return out;
}

// Formats a number compactly: integers as-is, reals to 3 dp without trailing
// zeros, and non-finite values verbatim.
export function fmtNum(n: number): string {
  if (!isFinite(n)) return String(n);
  if (Number.isInteger(n)) return String(n);
  return n.toFixed(3).replace(/\.?0+$/, '');
}
