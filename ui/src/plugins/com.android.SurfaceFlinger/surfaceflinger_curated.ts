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

// Derives the "curated properties" summary for a selected layer from the
// flattened proto args. Mirrors the field set the layer property groups +
// surface_flinger_property_groups + visibility summary show, so users get the
// same at-a-glance view.

import {fmtNum, type SfArg, type SfLayer} from './surfaceflinger_data';

export interface CuratedRow {
  label: string;
  value: string;
  // Layer ids this row references (occludedBy/relativeParent), for click-to-go.
  layerRefs?: number[];
}

export interface CuratedSection {
  title: string;
  rows: CuratedRow[];
}

export function buildArgMaps(args: SfArg[]): {
  byKey: Map<string, SfArg>;
  byFlat: Map<string, SfArg[]>;
} {
  const byKey = new Map<string, SfArg>();
  const byFlat = new Map<string, SfArg[]>();
  for (const a of args) {
    byKey.set(a.key, a);
    const list = byFlat.get(a.flatKey);
    if (list) list.push(a);
    else byFlat.set(a.flatKey, [a]);
  }
  return {byKey, byFlat};
}

function s(byKey: Map<string, SfArg>, key: string): string | undefined {
  const a = byKey.get(key);
  if (a === undefined) return undefined;
  if (a.type === 'real') return fmtNum(a.value as number);
  return String(a.value);
}

function rect(byKey: Map<string, SfArg>, p: string): string | undefined {
  const l = byKey.get(`${p}.left`);
  const t = byKey.get(`${p}.top`);
  const r = byKey.get(`${p}.right`);
  const b = byKey.get(`${p}.bottom`);
  if (!l && !t && !r && !b) return undefined;
  const f = (a?: SfArg) => (a ? fmtNum(Number(a.value)) : '?');
  return `(${f(l)}, ${f(t)}) – (${f(r)}, ${f(b)})`;
}

function color(byKey: Map<string, SfArg>, p: string): string | undefined {
  const r = byKey.get(`${p}.r`);
  const g = byKey.get(`${p}.g`);
  const bl = byKey.get(`${p}.b`);
  const a = byKey.get(`${p}.a`);
  if (!r && !g && !bl && !a) return undefined;
  const f = (x?: SfArg) => (x ? fmtNum(Number(x.value)) : '?');
  return `r:${f(r)} g:${f(g)} b:${f(bl)} α:${f(a)}`;
}

// Corner radii are shown as "(tl, tr, bl, br)", falling back to the scalar
// corner_radius (replicated to all four) when the per-corner array is absent.
function corners(
  byKey: Map<string, SfArg>,
  arrayPrefix: string,
  scalarKey: string,
): string | undefined {
  const tl = byKey.get(`${arrayPrefix}.tl`);
  const tr = byKey.get(`${arrayPrefix}.tr`);
  const bl = byKey.get(`${arrayPrefix}.bl`);
  const br = byKey.get(`${arrayPrefix}.br`);
  const f = (x?: SfArg) => (x ? fmtNum(Number(x.value)) : '0');
  if (tl || tr || bl || br) return `(${f(tl)}, ${f(tr)}, ${f(bl)}, ${f(br)})`;
  const sc = byKey.get(scalarKey);
  if (sc && Number(sc.value) !== 0) {
    const v = fmtNum(Number(sc.value));
    return `(${v}, ${v}, ${v}, ${v})`;
  }
  return undefined;
}

// Pixel-valued properties get a " px" suffix (shadow/blur radius).
function px(byKey: Map<string, SfArg>, key: string): string | undefined {
  const v = s(byKey, key);
  return v === undefined ? undefined : `${v} px`;
}

function transform(byKey: Map<string, SfArg>, p: string): string | undefined {
  const v = (k: string) => byKey.get(`${p}.${k}`);
  if (!v('dsdx') && !v('dsdy') && !v('tx') && !v('ty')) return undefined;
  const f = (x?: SfArg) => (x ? fmtNum(Number(x.value)) : '0');
  return (
    `[${f(v('dsdx'))} ${f(v('dtdx'))} ${f(v('tx'))}]  ` +
    `[${f(v('dtdy'))} ${f(v('dsdy'))} ${f(v('ty'))}]`
  );
}

function push(rows: CuratedRow[], label: string, value?: string) {
  if (value !== undefined && value !== '') rows.push({label, value});
}

export function curatedProperties(
  layer: SfLayer,
  args: SfArg[],
  layerName: (id: number) => string,
): CuratedSection[] {
  const {byKey, byFlat} = buildArgMaps(args);
  const sections: CuratedSection[] = [];

  // ----- Visibility -----
  const vis: CuratedRow[] = [];
  vis.push({label: 'Visible', value: layer.isVisible ? 'true' : 'false'});
  const reasons = (byFlat.get('visibility_reason') ?? []).map((a) =>
    String(a.value),
  );
  if (reasons.length) push(vis, 'Invisible due to', reasons.join(', '));
  const refRow = (flat: string, label: string) => {
    const ids = (byFlat.get(flat) ?? []).map((a) => Number(a.value));
    if (ids.length) {
      vis.push({
        label,
        value: ids.map((id) => `${layerName(id)} (#${id})`).join(', '),
        layerRefs: ids,
      });
    }
  };
  refRow('occluded_by', 'Occluded by');
  refRow('partially_occluded_by', 'Partially occluded by');
  refRow('covered_by', 'Covered by');
  push(vis, 'Flags', s(byKey, 'flags')); // flags shown in the visibility group
  sections.push({title: 'Visibility', rows: vis});

  // ----- Geometry -----
  const geo: CuratedRow[] = [];
  push(geo, 'Z', s(byKey, 'z'));
  push(geo, 'Layer stack', s(byKey, 'layer_stack'));
  if (layer.zOrderRelativeOf > 0) {
    geo.push({
      label: 'Z relative to',
      value: `${layerName(layer.zOrderRelativeOf)} (#${layer.zOrderRelativeOf})`,
      layerRefs: [layer.zOrderRelativeOf],
    });
  }
  push(geo, 'Bounds', rect(byKey, 'bounds'));
  push(geo, 'Screen bounds', rect(byKey, 'screen_bounds'));
  push(geo, 'Crop', rect(byKey, 'crop'));
  push(geo, 'Destination frame', rect(byKey, 'destination_frame'));
  push(geo, 'Transform', transform(byKey, 'transform'));
  push(geo, 'Requested transform', transform(byKey, 'requested_transform'));
  sections.push({title: 'Geometry', rows: geo});

  // ----- Buffer -----
  const buf: CuratedRow[] = [];
  const bw = s(byKey, 'active_buffer.width');
  const bh = s(byKey, 'active_buffer.height');
  if (bw || bh) {
    const stride = s(byKey, 'active_buffer.stride');
    const fmt = s(byKey, 'active_buffer.format');
    buf.push({
      label: 'Active buffer',
      value: `${bw ?? '?'} × ${bh ?? '?'}  stride ${stride ?? '?'}  format ${fmt ?? '?'}`,
    });
  }
  push(buf, 'Buffer transform', s(byKey, 'buffer_transform.type'));
  push(buf, 'Dataspace', s(byKey, 'dataspace'));
  push(buf, 'Frame number', s(byKey, 'curr_frame'));
  push(
    buf,
    'HWC composition',
    s(byKey, 'hwc_composition_type') ?? s(byKey, 'composition_type'),
  );
  if (buf.length) sections.push({title: 'Buffer', rows: buf});

  // ----- Effects -----
  const fx: CuratedRow[] = [];
  push(fx, 'Color', color(byKey, 'color'));
  push(fx, 'Requested color', color(byKey, 'requested_color'));
  push(fx, 'Corner radii', corners(byKey, 'corner_radii', 'corner_radius'));
  push(
    fx,
    'Requested corner radii',
    corners(byKey, 'requested_corner_radii', 'requested_corner_radius'),
  );
  push(fx, 'Corner radius crop', rect(byKey, 'corner_radius_crop'));
  push(fx, 'Shadow radius', px(byKey, 'shadow_radius'));
  push(fx, 'Background blur', px(byKey, 'background_blur_radius'));
  if (fx.length) sections.push({title: 'Color & effects', rows: fx});

  // ----- Input -----
  const inp: CuratedRow[] = [];
  push(inp, 'Focusable', s(byKey, 'input_window_info.focusable'));
  push(
    inp,
    'Touchable region',
    rect(byKey, 'input_window_info.touchable_region.rect') ??
      rect(byKey, 'input_window_info.frame'),
  );
  push(inp, 'Input transform', transform(byKey, 'input_window_info.transform'));
  push(inp, 'Input config', s(byKey, 'input_window_info.input_config'));
  push(inp, 'Crop layer (touch)', s(byKey, 'input_window_info.crop_layer_id'));
  push(
    inp,
    'Replace touch with crop',
    s(byKey, 'input_window_info.replace_touchable_region_with_crop'),
  );
  if (inp.length) sections.push({title: 'Input', rows: inp});

  return sections.filter((sec) => sec.rows.length > 0);
}
