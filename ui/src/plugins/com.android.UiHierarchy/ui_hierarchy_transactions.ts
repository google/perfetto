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

// SurfaceFlinger transactions between two snapshots: which process changed
// a layer, what it changed, and which layers it changed together with it.
// They explain the property changes the diff shows.

import {Duration} from '../../base/time';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  displayName,
  formatNumber,
  type PropLink,
  type PropRow,
  PropRowsBuilder,
  type PropSection,
  type PropValue,
} from './ui_hierarchy_props';
import {decodeLayerFlags, decodeTransformType} from './ui_hierarchy_sf';
import {type Transition, transitionTypeName} from './ui_hierarchy_transitions';

// What happened to the layer.
export type LayerTransactionKind =
  'changed' | 'added' | 'destroyed' | 'handle_destroyed';

// One layer's part of a transaction, or a layer lifecycle event.
export interface LayerTransaction {
  readonly ts: bigint;
  readonly layerId: number;
  readonly kind: LayerTransactionKind;
  // Lifecycle events have none.
  readonly transactionId?: bigint;
  // The process that applied the transaction.
  readonly pid?: number;
  // The changed fields, as layer_state_t::what flags ('ePositionChanged').
  readonly changes: ReadonlyArray<string>;
}

// The transactions committed in (after, upTo].
export interface TransactionLog {
  readonly after: bigint;
  readonly upTo: bigint;
  readonly entries: ReadonlyArray<LayerTransaction>;
  // Process names by pid, for the senders.
  readonly processNames: ReadonlyMap<number, string>;
  // Whether entries stop at MAX_LOG_ENTRIES.
  readonly truncated: boolean;
}

export const MAX_LOG_ENTRIES = 20000;

// How many groups or layers a section lists before summarising the rest.
const MAX_GROUPS = 8;
const MAX_APPLIED_WITH = 6;

const KINDS: ReadonlyMap<string, LayerTransactionKind> = new Map([
  ['LAYER_CHANGED', 'changed'],
  ['LAYER_ADDED', 'added'],
  ['LAYER_DESTROYED', 'destroyed'],
  ['LAYER_HANDLE_DESTROYED', 'handle_destroyed'],
]);

const KIND_NAMES: ReadonlyMap<LayerTransactionKind, string> = new Map([
  ['changed', 'Changed'],
  ['added', 'Created'],
  ['destroyed', 'Destroyed'],
  ['handle_destroyed', 'Handle released'],
]);

// Flags whose names don't read well once 'e' and 'Changed' are dropped.
const CHANGE_NAMES: ReadonlyMap<string, string> = new Map([
  ['eLayerChanged', 'Z'],
  ['eRelativeLayerChanged', 'Relative Z'],
  ['eReparent', 'Parent'],
  ['eHasListenerCallbacksChanged', 'Listener callbacks'],
]);

// Set with each new buffer; shown as part of 'Buffer' when it changed.
const BUFFER_UPDATE_FLAGS: ReadonlySet<string> = new Set([
  'eAcquireFenceChanged',
  'eAutoRefreshChanged',
  'eBufferCropChanged',
  'eBufferTransformChanged',
  'eCachedBufferChanged',
  'eDataspaceChanged',
  'eDestinationFrameChanged',
  'eHasListenerCallbacksChanged',
  'eHdrMetadataChanged',
  'eSurfaceDamageRegionChanged',
  'eTransformToDisplayInverseChanged',
]);

// A layer_state_t::what flag as words: 'ePositionChanged' -> 'Position',
// 'eBackgroundBlurRadiusChanged' -> 'Background blur radius'.
export function changeName(flag: string): string {
  const special = CHANGE_NAMES.get(flag);
  if (special !== undefined) return special;
  const core = flag.replace(/^e(?=[A-Z])/, '').replace(/Changed$/, '');
  const words = core.split(/(?=[A-Z])/).filter((w) => w !== '');
  if (words.length === 0) return flag;
  return [words[0], ...words.slice(1).map((w) => w.toLowerCase())].join(' ');
}

// The changed fields worth reading, in flag order: with a new buffer, the
// fields set along with every buffer are left to 'Buffer'.
export function changeSummary(changes: ReadonlyArray<string>): string[] {
  const hasBuffer = changes.includes('eBufferChanged');
  return changes
    .filter((c) => !hasBuffer || !BUFFER_UPDATE_FLAGS.has(c))
    .map(changeName);
}

// The sender's name for a row label: the last part of a package name
// ('com.android.systemui' -> 'systemui'), else the process name or pid.
export function senderLabel(
  pid: number | undefined,
  processNames: ReadonlyMap<number, string>,
): string {
  if (pid === undefined) return 'Unknown sender';
  const name = processNames.get(pid);
  if (name === undefined) return `pid ${pid}`;
  const m = /^[a-z]\w*(?:\.\w+)*\.(\w+)$/.exec(name);
  return m !== null ? m[1] : name;
}

function senderTitle(
  pid: number | undefined,
  processNames: ReadonlyMap<number, string>,
): string {
  if (pid === undefined) return 'Unknown sender';
  const name = processNames.get(pid);
  return name !== undefined ? `${name} (${pid})` : `pid ${pid}`;
}

// A transaction starting or finishing a shell transition.
export interface TransitionRole {
  readonly label: string; // 'start of OPEN'
  readonly transitionId: number;
}

// The start and finish transactions of `transitions`, by transaction id.
export function transitionRoles(
  transitions: ReadonlyArray<Transition>,
): Map<bigint, TransitionRole> {
  const roles = new Map<bigint, TransitionRole>();
  for (const t of transitions) {
    const type = transitionTypeName(t.type);
    if (t.startTransactionId !== undefined) {
      roles.set(t.startTransactionId, {
        label: `start of ${type}`,
        transitionId: t.id,
      });
    }
    if (t.finishTransactionId !== undefined) {
      roles.set(t.finishTransactionId, {
        label: `finish of ${type}`,
        transitionId: t.id,
      });
    }
  }
  return roles;
}

// The changes one process made with the same fields (and transition role).
export interface TransactionGroup {
  readonly pid?: number;
  readonly summary: ReadonlyArray<string>;
  // Every changed field, for the tooltip.
  readonly changes: ReadonlyArray<string>;
  readonly role?: string;
  // Distinct transactions.
  readonly count: number;
  readonly lastTs: bigint;
  // The transaction ids, latest first.
  readonly ids: ReadonlyArray<bigint>;
}

// Groups the changes to `layerIds` by sender, changed fields and
// transition role, latest first. A transaction changing several of the
// layers counts once.
export function groupLayerTransactions(
  entries: ReadonlyArray<LayerTransaction>,
  layerIds: ReadonlySet<number>,
  roles: ReadonlyMap<bigint, TransitionRole>,
): TransactionGroup[] {
  interface Acc {
    pid?: number;
    summary: string[];
    changes: Set<string>;
    role?: string;
    // Latest commit time by transaction id.
    ids: Map<bigint, bigint>;
    unnamed: number;
    lastTs: bigint;
  }
  const groups = new Map<string, Acc>();
  for (const e of entries) {
    if (e.kind !== 'changed' || !layerIds.has(e.layerId)) continue;
    const role =
      e.transactionId !== undefined
        ? roles.get(e.transactionId)?.label
        : undefined;
    const summary = changeSummary(e.changes);
    const key = [e.pid ?? '', role ?? '', ...summary].join('|');
    let g = groups.get(key);
    if (g === undefined) {
      g = {
        pid: e.pid,
        summary,
        changes: new Set(),
        role,
        ids: new Map(),
        unnamed: 0,
        lastTs: e.ts,
      };
      groups.set(key, g);
    }
    for (const c of e.changes) g.changes.add(c);
    if (e.transactionId !== undefined) {
      const seen = g.ids.get(e.transactionId);
      if (seen === undefined || e.ts > seen) g.ids.set(e.transactionId, e.ts);
    } else {
      g.unnamed++;
    }
    if (e.ts > g.lastTs) g.lastTs = e.ts;
  }
  return [...groups.values()]
    .map((g) => ({
      pid: g.pid,
      summary: g.summary,
      changes: [...g.changes],
      role: g.role,
      count: g.ids.size + g.unnamed,
      lastTs: g.lastTs,
      ids: [...g.ids.entries()]
        .sort((a, b) => compareDesc(a[1], b[1]))
        .map(([id]) => id),
    }))
    .sort((a, b) => compareDesc(a.lastTs, b.lastTs));
}

function compareDesc(a: bigint, b: bigint): number {
  return a === b ? 0 : a > b ? -1 : 1;
}

// The other layers the transactions of `layerIds` changed, by how many
// transactions they shared, most first.
export function appliedWith(
  entries: ReadonlyArray<LayerTransaction>,
  layerIds: ReadonlySet<number>,
): Array<{readonly layerId: number; readonly count: number}> {
  const ids = new Set<bigint>();
  for (const e of entries) {
    if (e.transactionId !== undefined && layerIds.has(e.layerId)) {
      ids.add(e.transactionId);
    }
  }
  const shared = new Map<number, Set<bigint>>();
  for (const e of entries) {
    if (
      e.transactionId === undefined ||
      layerIds.has(e.layerId) ||
      !ids.has(e.transactionId)
    ) {
      continue;
    }
    let s = shared.get(e.layerId);
    if (s === undefined) {
      s = new Set();
      shared.set(e.layerId, s);
    }
    s.add(e.transactionId);
  }
  return [...shared.entries()]
    .map(([layerId, s]) => ({layerId, count: s.size}))
    .sort((a, b) => b.count - a.count || a.layerId - b.layerId);
}

export interface TransactionSectionOpts {
  // The snapshot the log starts after, e.g. 'snapshot 14 · previous'.
  readonly since: string;
  readonly transitions: ReadonlyArray<Transition>;
  readonly layerName: (layerId: number) => string | undefined;
  // What the transactions set, once loaded: makes the rows expandable.
  readonly details?: TransactionDetails;
}

function plural(n: number, what: string): string {
  return `${n} ${what}${n === 1 ? '' : 's'}`;
}

function sinceRows(log: TransactionLog, since: string): PropRowsBuilder {
  return new PropRowsBuilder().text('Since', since, {
    title: `Transactions committed in the ${Duration.humanise(
      log.upTo - log.after,
    )} before this snapshot`,
  });
}

function truncatedRow(b: PropRowsBuilder, log: TransactionLog): void {
  if (log.truncated) {
    b.text('Truncated', `first ${MAX_LOG_ENTRIES} layer changes only`);
  }
}

// The transactions in `log` that changed `layerIds` (a layer, or the
// leash, container and buffer layers of a window): one row per sender and
// changed fields, then the layers changed together with them.
export function layerTransactionRows(
  log: TransactionLog,
  layerIds: ReadonlySet<number>,
  opts: TransactionSectionOpts,
): PropRow[] {
  const b = sinceRows(log, opts.since);
  const lifecycle: string[] = [];
  for (const e of log.entries) {
    if (e.kind === 'changed' || !layerIds.has(e.layerId)) continue;
    const name = KIND_NAMES.get(e.kind) ?? e.kind;
    // A layer's lifecycle events can be logged more than once.
    if (!lifecycle.includes(name)) lifecycle.push(name);
  }
  b.text('Lifecycle', lifecycle.join(', '));

  const groups = groupLayerTransactions(
    log.entries,
    layerIds,
    transitionRoles(opts.transitions),
  );
  if (groups.length === 0 && lifecycle.length === 0) {
    b.text('Changes', 'none');
  }
  for (const g of groups.slice(0, MAX_GROUPS)) {
    const parts = [g.summary.join(', ') || 'No fields'];
    if (g.count > 1) parts.push(`×${g.count}`);
    if (g.role !== undefined) parts.push(g.role);
    const title = [
      senderTitle(g.pid, log.processNames),
      `Last ${Duration.humanise(log.upTo - g.lastTs)} before the snapshot`,
      g.changes.join(' | '),
    ].join('\n');
    b.row({
      label: senderLabel(g.pid, log.processNames),
      value: {kind: 'text', text: parts.join(' · '), title},
      children:
        opts.details !== undefined
          ? groupDetailRows(g, layerIds, log, opts.details, opts)
          : undefined,
    });
  }
  const rest = groups.slice(MAX_GROUPS);
  if (rest.length > 0) {
    const n = rest.reduce((sum, g) => sum + g.count, 0);
    b.text('More', `${plural(n, 'transaction')} in ${rest.length} groups`);
  }

  const others = appliedWith(log.entries, layerIds);
  const links: PropLink[] = others.slice(0, MAX_APPLIED_WITH).map((o) => {
    const name = opts.layerName(o.layerId);
    return {
      text: name !== undefined ? displayName(name) : `#${o.layerId}`,
      title: `${name ?? `Layer ${o.layerId}`}: ${plural(
        o.count,
        'shared transaction',
      )}`,
      target: {kind: 'layer', layerId: o.layerId},
    };
  });
  b.links('Applied with', links);
  if (others.length > MAX_APPLIED_WITH) {
    b.text('More layers', `${others.length - MAX_APPLIED_WITH}`);
  }
  truncatedRow(b, log);
  return b.build();
}

// The processes that sent transactions in `log`, busiest first.
export function senderRows(log: TransactionLog, since: string): PropRow[] {
  const bySender = new Map<
    number | undefined,
    {ids: Set<bigint>; layers: Set<number>}
  >();
  for (const e of log.entries) {
    if (e.kind !== 'changed' || e.transactionId === undefined) continue;
    let s = bySender.get(e.pid);
    if (s === undefined) {
      s = {ids: new Set(), layers: new Set()};
      bySender.set(e.pid, s);
    }
    s.ids.add(e.transactionId);
    s.layers.add(e.layerId);
  }
  const b = sinceRows(log, since);
  const senders = [...bySender.entries()].sort(
    (a, b) =>
      b[1].ids.size - a[1].ids.size || b[1].layers.size - a[1].layers.size,
  );
  if (senders.length === 0) b.text('Changes', 'none');
  for (const [pid, s] of senders.slice(0, MAX_GROUPS)) {
    b.text(
      senderLabel(pid, log.processNames),
      `${plural(s.ids.size, 'transaction')} · ${plural(s.layers.size, 'layer')}`,
      {title: senderTitle(pid, log.processNames)},
    );
  }
  if (senders.length > MAX_GROUPS) {
    b.text('More', plural(senders.length - MAX_GROUPS, 'process'));
  }
  truncatedRow(b, log);
  return b.build();
}

export function transactionSection(rows: PropRow[]): PropSection {
  return {title: 'Transactions', rows};
}

// Details ---------------------------------------------------------------------

// What one transaction set on one layer.
export interface LayerChange {
  readonly layerId: number;
  readonly changes: ReadonlyArray<string>;
  // The layer_state_t fields it set, by arg key ('x', 'crop.left', ...).
  readonly values: ReadonlyMap<string, string>;
}

export interface TransactionDetail {
  readonly id: bigint;
  readonly ts: bigint;
  readonly vsyncId?: number;
  readonly pid?: number;
  readonly layers: ReadonlyArray<LayerChange>;
}

// The latest transactions that changed some layers, with everything they
// set on every layer.
export interface TransactionDetails {
  readonly byId: ReadonlyMap<bigint, TransactionDetail>;
}

// How many transactions a group lists, and how many layers a transaction.
const MAX_GROUP_TRANSACTIONS = 20;
const MAX_TRANSACTION_LAYERS = 20;
export const MAX_DETAIL_TRANSACTIONS = 200;

// SF writes these ids when there is no layer.
const NO_LAYER_IDS: ReadonlySet<string> = new Set(['-1', '4294967295']);

// Not shown per layer: the layer itself, the flags that say which fields
// are set, and the token that orders transactions from one sender.
const HIDDEN_KEYS: ReadonlySet<string> = new Set([
  'layer_id',
  'what',
  'apply_token',
]);

type ValueRow = (
  v: ReadonlyMap<string, string>,
  layerName: (layerId: number) => string | undefined,
) => PropRow | undefined;

function num(v: string | undefined): string | undefined {
  if (v === undefined) return undefined;
  const n = Number(v);
  return Number.isFinite(n) ? formatNumber(n) : v;
}

function textRow(label: string, text: string | undefined): PropRow | undefined {
  return text !== undefined ? {label, value: {kind: 'text', text}} : undefined;
}

function boundsOf(
  v: ReadonlyMap<string, string>,
  prefix: string,
): string | undefined {
  const sides = ['left', 'top', 'right', 'bottom'].map((s) =>
    num(v.get(`${prefix}.${s}`)),
  );
  if (sides.every((s) => s === undefined)) return undefined;
  return `[${sides.map((s) => s ?? '?').join(', ')}]`;
}

function layerLink(
  label: string,
  id: string | undefined,
  layerName: (layerId: number) => string | undefined,
): PropRow | undefined {
  if (id === undefined) return undefined;
  const layerId = Number(id);
  if (NO_LAYER_IDS.has(id) || !Number.isInteger(layerId)) {
    return textRow(label, 'none');
  }
  return layerRow(label, layerId, layerName);
}

function layerRow(
  label: string,
  layerId: number,
  layerName: (layerId: number) => string | undefined,
): PropRow {
  const name = layerName(layerId);
  return {
    label,
    value: {
      kind: 'links',
      links: [
        {
          text: name !== undefined ? displayName(name) : `#${layerId}`,
          title: name,
          target: {kind: 'layer', layerId},
        },
      ],
    },
  };
}

// The fields a layer change can set, curated, with the arg keys each
// reads. Keys left over are listed raw.
const VALUE_ROWS: ReadonlyArray<readonly [ReadonlyArray<string>, ValueRow]> = [
  [
    ['x', 'y'],
    (v) =>
      v.has('x') || v.has('y')
        ? textRow(
            'Position',
            `(${num(v.get('x')) ?? '?'}, ${num(v.get('y')) ?? '?'})`,
          )
        : undefined,
  ],
  [['z'], (v) => textRow('Z', num(v.get('z')))],
  [['parent_id'], (v, n) => layerLink('Parent', v.get('parent_id'), n)],
  [
    ['relative_parent_id'],
    (v, n) => layerLink('Relative to', v.get('relative_parent_id'), n),
  ],
  [['alpha'], (v) => textRow('Alpha', num(v.get('alpha')))],
  [
    ['matrix.dsdx', 'matrix.dtdx', 'matrix.dtdy', 'matrix.dsdy'],
    (v) => {
      const m = ['dsdx', 'dtdx', 'dtdy', 'dsdy'].map((k) =>
        num(v.get(`matrix.${k}`)),
      );
      if (m.every((x) => x === undefined)) return undefined;
      const [a, b, c, d] = m.map((x) => x ?? '?');
      return textRow('Matrix', `[${a}, ${b}; ${c}, ${d}]`);
    },
  ],
  [
    ['crop.left', 'crop.top', 'crop.right', 'crop.bottom'],
    (v) => textRow('Crop', boundsOf(v, 'crop')),
  ],
  [
    [
      'corner_radius',
      'corner_radii.tl',
      'corner_radii.tr',
      'corner_radii.br',
      'corner_radii.bl',
    ],
    (v) => {
      const radii = ['tl', 'tr', 'br', 'bl'].map((k) =>
        num(v.get(`corner_radii.${k}`)),
      );
      const same = radii.every((r) => r === radii[0]);
      const text =
        num(v.get('corner_radius')) ??
        (radii[0] === undefined
          ? undefined
          : same
            ? radii[0]
            : radii.map((r) => r ?? '?').join(', '));
      return textRow(
        'Corner radius',
        text !== undefined ? `${text} px` : undefined,
      );
    },
  ],
  [
    ['flags', 'mask'],
    (v) => {
      const mask = Number(v.get('mask') ?? v.get('flags'));
      const flags = Number(v.get('flags') ?? 0);
      if (!Number.isInteger(mask) || mask === 0) return undefined;
      const parts: string[] = [];
      for (let bit = 1; bit !== 0 && bit <= mask; bit <<= 1) {
        if ((mask & bit) === 0) continue;
        parts.push(`${decodeLayerFlags(bit)[0]} ${flags & bit ? 'on' : 'off'}`);
      }
      return textRow('Flags', parts.join(', '));
    },
  ],
  [
    [
      'buffer_data.width',
      'buffer_data.height',
      'buffer_data.pixel_format',
      'buffer_data.frame_number',
    ],
    (v) => {
      const w = v.get('buffer_data.width');
      const h = v.get('buffer_data.height');
      const parts: string[] = [];
      if (w !== undefined && h !== undefined) parts.push(`${w}×${h}`);
      const format = v.get('buffer_data.pixel_format');
      if (format !== undefined) {
        parts.push(format.replace(/^PIXEL_FORMAT_/, ''));
      }
      const frame = v.get('buffer_data.frame_number');
      if (frame !== undefined) parts.push(`frame ${frame}`);
      return textRow(
        'Buffer',
        parts.length > 0 ? parts.join(' · ') : undefined,
      );
    },
  ],
  [
    [
      'destination_frame.left',
      'destination_frame.top',
      'destination_frame.right',
      'destination_frame.bottom',
    ],
    (v) => textRow('Destination frame', boundsOf(v, 'destination_frame')),
  ],
  [
    [
      'buffer_crop.left',
      'buffer_crop.top',
      'buffer_crop.right',
      'buffer_crop.bottom',
    ],
    (v) => {
      const b = boundsOf(v, 'buffer_crop');
      // An empty buffer crop means the whole buffer.
      return textRow('Buffer crop', b === '[0, 0, 0, 0]' ? undefined : b);
    },
  ],
  [
    ['transform'],
    (v) => {
      const t = Number(v.get('transform') ?? 0);
      return textRow(
        'Transform',
        t !== 0 ? decodeTransformType(t).join(' | ') : undefined,
      );
    },
  ],
  [
    ['color.r', 'color.g', 'color.b'],
    (v) =>
      v.has('color.r')
        ? textRow(
            'Color',
            `(${['r', 'g', 'b'].map((c) => num(v.get(`color.${c}`)) ?? '?').join(', ')})`,
          )
        : undefined,
  ],
  [
    ['shadow_radius'],
    (v) => textRow('Shadow radius', num(v.get('shadow_radius'))),
  ],
  [
    ['background_blur_radius'],
    (v) => textRow('Blur radius', num(v.get('background_blur_radius'))),
  ],
  [
    ['background_blur_scale'],
    (v) => textRow('Blur scale', num(v.get('background_blur_scale'))),
  ],
  [['layer_stack'], (v) => textRow('Layer stack', v.get('layer_stack'))],
  [
    ['frame_rate', 'frame_rate_compatibility', 'change_frame_rate_strategy'],
    (v) => textRow('Frame rate', num(v.get('frame_rate'))),
  ],
  [
    ['is_trusted_overlay'],
    (v) => textRow('Trusted overlay', v.get('is_trusted_overlay')),
  ],
];

const CURATED_KEYS: ReadonlySet<string> = new Set(
  VALUE_ROWS.flatMap(([keys]) => keys),
);

// What `change` set: the curated fields, then the other fields raw.
export function layerChangeRows(
  change: LayerChange,
  layerName: (layerId: number) => string | undefined,
): PropRow[] {
  const rows: PropRow[] = [];
  for (const [, row] of VALUE_ROWS) {
    const r = row(change.values, layerName);
    if (r !== undefined) rows.push(r);
  }
  const raw = [...change.values.entries()]
    .filter(([k]) => !CURATED_KEYS.has(k) && !HIDDEN_KEYS.has(k))
    .sort(([a], [b]) => a.localeCompare(b));
  if (raw.length > 0) {
    rows.push({
      label: 'Other fields',
      value: {kind: 'text', text: `${raw.length}`},
      children: raw.map(([k, value]) => ({
        label: k,
        value: {kind: 'text', text: value, mono: true},
      })),
    });
  }
  return rows;
}

// The first curated values of `change`, e.g. 'Position (0, 1240) · Alpha
// 0.6', else its changed fields.
export function layerChangeSummary(
  change: LayerChange,
  layerName: (layerId: number) => string | undefined,
  max = 3,
): string {
  const parts = layerChangeRows(change, layerName)
    .filter((r) => r.children === undefined)
    .map((r) => {
      const v = r.value;
      const text =
        v.kind === 'links'
          ? v.links.map((l) => l.text).join(', ')
          : valueText(v);
      return `${r.label} ${text}`;
    });
  if (parts.length === 0) return changeSummary(change.changes).join(', ');
  return parts.length > max
    ? `${parts.slice(0, max).join(' · ')} · …`
    : parts.join(' · ');
}

function valueText(v: PropValue): string {
  switch (v.kind) {
    case 'text':
    case 'copy':
    case 'chip':
      return v.text;
    case 'links':
      return v.links.map((l) => l.text).join(', ');
    case 'list':
      return v.items.join(', ');
  }
}

// One transaction of a group: when, what it set on the selected layers,
// and expanded, its ids and every layer it changed.
function transactionRow(
  t: TransactionDetail,
  layerIds: ReadonlySet<number>,
  log: TransactionLog,
  roles: ReadonlyMap<bigint, TransitionRole>,
  layerName: (layerId: number) => string | undefined,
): PropRow {
  const own = t.layers.filter((l) => layerIds.has(l.layerId));
  const others = t.layers.filter((l) => !layerIds.has(l.layerId));
  const role = roles.get(t.id);
  const children = new PropRowsBuilder()
    .copy('Id', `${t.id}`)
    .text('Sender', senderTitle(t.pid, log.processNames))
    .text('Vsync', t.vsyncId !== undefined ? `${t.vsyncId}` : undefined);
  if (role !== undefined) {
    children.links('Transition', [
      {
        text: role.label,
        title: 'Compare across this transition',
        target: {kind: 'transition', transitionId: role.transitionId},
      },
    ]);
  }
  for (const l of [...own, ...others].slice(0, MAX_TRANSACTION_LAYERS)) {
    const name = layerName(l.layerId);
    children.row({
      label: name !== undefined ? displayName(name) : `#${l.layerId}`,
      value: {
        kind: 'text',
        text: layerChangeSummary(l, layerName),
        title: name,
      },
      children: [
        layerRow('Layer', l.layerId, layerName),
        ...layerChangeRows(l, layerName),
      ],
    });
  }
  if (t.layers.length > MAX_TRANSACTION_LAYERS) {
    children.text('More layers', `${t.layers.length - MAX_TRANSACTION_LAYERS}`);
  }
  const summary = own
    .map((l) => layerChangeSummary(l, layerName))
    .filter((s) => s !== '')
    .join(' | ');
  return {
    label: `−${Duration.humanise(log.upTo - t.ts)}`,
    value: {
      kind: 'text',
      text: summary !== '' ? summary : 'No fields',
      title: `Committed ${Duration.humanise(log.upTo - t.ts)} before the snapshot`,
    },
    children: children.build(),
  };
}

// The transactions of group `g`, latest first.
function groupDetailRows(
  g: TransactionGroup,
  layerIds: ReadonlySet<number>,
  log: TransactionLog,
  details: TransactionDetails,
  opts: TransactionSectionOpts,
): PropRow[] {
  const roles = transitionRoles(opts.transitions);
  const rows: PropRow[] = [];
  let missing = 0;
  for (const id of g.ids) {
    const t = details.byId.get(id);
    if (t === undefined || rows.length >= MAX_GROUP_TRANSACTIONS) {
      missing++;
      continue;
    }
    rows.push(transactionRow(t, layerIds, log, roles, opts.layerName));
  }
  if (missing > 0) {
    rows.push({
      label: 'Older',
      value: {
        kind: 'text',
        text: `${plural(missing, 'transaction')}: see Query`,
      },
    });
  }
  return rows;
}

// SQL listing everything the transactions in (after, upTo] set on
// `layerIds`, latest first, for the query results tab.
export function transactionsQuery(
  layerIds: ReadonlySet<number>,
  after: bigint,
  upTo: bigint,
): string {
  return `-- SurfaceFlinger transactions on layers ${[...layerIds].join(', ')}
SELECT
  s.ts,
  s.vsync_id,
  t.transaction_id,
  coalesce(nullif(t.pid, 0), t.transaction_id >> 32) AS pid,
  p.name AS process,
  t.layer_id,
  t.transaction_type,
  a.key,
  a.display_value AS value
FROM __intrinsic_surfaceflinger_transactions s
JOIN __intrinsic_surfaceflinger_transaction t ON t.snapshot_id = s.id
LEFT JOIN args a ON a.arg_set_id = t.arg_set_id
LEFT JOIN (
  SELECT pid, max(upid) AS upid FROM process GROUP BY pid
) pp ON pp.pid = coalesce(nullif(t.pid, 0), t.transaction_id >> 32)
LEFT JOIN process p ON p.upid = pp.upid
WHERE s.ts > ${after} AND s.ts <= ${upTo}
  AND t.layer_id IN (${[...layerIds].join(', ')})
ORDER BY s.ts DESC, t.id, a.key`;
}

// The latest MAX_DETAIL_TRANSACTIONS transactions in (after, upTo] that
// changed `layerIds`, with what they set on every layer.
export async function queryTransactionDetails(
  engine: Engine,
  after: bigint,
  upTo: bigint,
  layerIds: ReadonlySet<number>,
): Promise<TransactionDetails> {
  const byId = new Map<bigint, TransactionDetail>();
  if (layerIds.size === 0) return {byId};
  const res = await engine.query(`
    WITH ids AS (
      SELECT t.transaction_id AS id, max(s.ts) AS ts
      FROM __intrinsic_surfaceflinger_transactions s
      JOIN __intrinsic_surfaceflinger_transaction t ON t.snapshot_id = s.id
      WHERE s.ts > ${after} AND s.ts <= ${upTo}
        AND t.layer_id IN (${[...layerIds].join(',')})
        AND t.transaction_type = 'LAYER_CHANGED'
        AND t.transaction_id IS NOT NULL AND t.transaction_id != 0
      GROUP BY 1
      ORDER BY 2 DESC
      LIMIT ${MAX_DETAIL_TRANSACTIONS}
    )
    SELECT
      s.ts,
      s.vsync_id,
      t.transaction_id,
      nullif(coalesce(nullif(t.pid, 0), t.transaction_id >> 32), 0) AS pid,
      t.layer_id,
      (
        SELECT group_concat(f.flag, '|')
        FROM __intrinsic_surfaceflinger_transaction_flag f
        WHERE f.flags_id = t.flags_id
      ) AS changes,
      a.key,
      a.display_value
    FROM ids
    JOIN __intrinsic_surfaceflinger_transaction t ON t.transaction_id = ids.id
    JOIN __intrinsic_surfaceflinger_transactions s ON s.id = t.snapshot_id
    LEFT JOIN args a ON a.arg_set_id = t.arg_set_id
    WHERE s.ts > ${after} AND s.ts <= ${upTo}
      AND t.transaction_type = 'LAYER_CHANGED'
    ORDER BY t.id
  `);
  interface Acc {
    id: bigint;
    ts: bigint;
    vsyncId?: number;
    pid?: number;
    layers: Map<number, {changes: string[]; values: Map<string, string>}>;
  }
  const acc = new Map<bigint, Acc>();
  const it = res.iter({
    ts: LONG,
    vsync_id: NUM_NULL,
    transaction_id: LONG,
    pid: NUM_NULL,
    layer_id: NUM,
    changes: STR_NULL,
    key: STR_NULL,
    display_value: STR_NULL,
  });
  for (; it.valid(); it.next()) {
    let t = acc.get(it.transaction_id);
    if (t === undefined) {
      t = {
        id: it.transaction_id,
        ts: it.ts,
        vsyncId: it.vsync_id ?? undefined,
        pid: it.pid ?? undefined,
        layers: new Map(),
      };
      acc.set(it.transaction_id, t);
    }
    let l = t.layers.get(it.layer_id);
    if (l === undefined) {
      l = {
        changes: it.changes !== null ? it.changes.split('|') : [],
        values: new Map(),
      };
      t.layers.set(it.layer_id, l);
    }
    if (it.key !== null && it.display_value !== null) {
      l.values.set(it.key, it.display_value);
    }
  }
  for (const t of acc.values()) {
    byId.set(t.id, {
      id: t.id,
      ts: t.ts,
      vsyncId: t.vsyncId,
      pid: t.pid,
      layers: [...t.layers.entries()].map(([layerId, l]) => ({
        layerId,
        changes: l.changes,
        values: l.values,
      })),
    });
  }
  return {byId};
}

// Whether the trace has SurfaceFlinger transactions.
export async function hasTransactions(engine: Engine): Promise<boolean> {
  const res = await engine.query(`
    SELECT EXISTS(
      SELECT 1 FROM __intrinsic_surfaceflinger_transactions
    ) AS has
  `);
  return res.firstRow({has: NUM}).has !== 0;
}

// The layer changes and lifecycle events committed in (after, upTo].
export async function queryTransactionLog(
  engine: Engine,
  after: bigint,
  upTo: bigint,
): Promise<TransactionLog> {
  // The sender is in the upper 32 bits of the transaction id when the
  // pid isn't recorded.
  const res = await engine.query(`
    SELECT
      s.ts,
      t.layer_id,
      t.transaction_type,
      t.transaction_id,
      nullif(coalesce(nullif(t.pid, 0), t.transaction_id >> 32), 0) AS pid,
      (
        SELECT group_concat(f.flag, '|')
        FROM __intrinsic_surfaceflinger_transaction_flag f
        WHERE f.flags_id = t.flags_id
      ) AS changes
    FROM __intrinsic_surfaceflinger_transactions s
    JOIN __intrinsic_surfaceflinger_transaction t ON t.snapshot_id = s.id
    WHERE s.ts > ${after} AND s.ts <= ${upTo}
      AND t.layer_id IS NOT NULL
      AND t.transaction_type IN (
        'LAYER_CHANGED', 'LAYER_ADDED', 'LAYER_DESTROYED',
        'LAYER_HANDLE_DESTROYED')
    ORDER BY s.ts, t.id
    LIMIT ${MAX_LOG_ENTRIES + 1}
  `);
  const entries: LayerTransaction[] = [];
  const pids = new Set<number>();
  const it = res.iter({
    ts: LONG,
    layer_id: NUM,
    transaction_type: STR,
    transaction_id: LONG_NULL,
    pid: NUM_NULL,
    changes: STR_NULL,
  });
  for (; it.valid() && entries.length < MAX_LOG_ENTRIES; it.next()) {
    const kind = KINDS.get(it.transaction_type);
    if (kind === undefined) continue;
    const pid = it.pid ?? undefined;
    if (pid !== undefined) pids.add(pid);
    entries.push({
      ts: it.ts,
      layerId: it.layer_id,
      kind,
      transactionId:
        it.transaction_id !== null && it.transaction_id !== 0n
          ? it.transaction_id
          : undefined,
      pid,
      changes: it.changes !== null ? it.changes.split('|') : [],
    });
  }
  return {
    after,
    upTo,
    entries,
    processNames: await queryProcessNames(engine, [...pids]),
    truncated: res.numRows() > MAX_LOG_ENTRIES,
  };
}

// The latest process with each pid.
async function queryProcessNames(
  engine: Engine,
  pids: ReadonlyArray<number>,
): Promise<Map<number, string>> {
  const names = new Map<number, string>();
  if (pids.length === 0) return names;
  const res = await engine.query(`
    SELECT pid, name
    FROM process
    WHERE pid IN (${pids.join(',')}) AND name IS NOT NULL
    ORDER BY upid
  `);
  for (const it = res.iter({pid: NUM, name: STR}); it.valid(); it.next()) {
    names.set(it.pid, it.name);
  }
  return names;
}
