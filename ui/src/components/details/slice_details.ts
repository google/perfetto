// Copyright (C) 2023 The Android Open Source Project
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

import m from 'mithril';
import {BigintMath} from '../../base/bigint_math';
import {exists} from '../../base/utils';
import type {SliceDetails} from '../sql_utils/slice';
import {Anchor} from '../../widgets/anchor';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  type BreakdownByThreadState,
  BreakdownByThreadStateTreeNode,
} from './thread_state';
import {DurationWidget} from '../widgets/duration';
import {renderProcessRef} from '../widgets/process';
import {renderThreadRef} from '../widgets/thread';
import {Timestamp} from '../widgets/timestamp';
import type {Trace} from '../../public/trace';
import {
  type DistributionPanelAttrs,
  openDistributionTab,
} from '../distribution_panel';
import {
  LONG,
  NUM,
  type Row,
  STR_NULL,
} from '../../trace_processor/query_result';
import {Time} from '../../base/time';
import {type Dataset, UnionDataset} from '../../trace_processor/dataset';
import {sqlValueToSqliteString} from '../../trace_processor/sql_utils';
import type {TreeExplorerOptionalAction} from '../../widgets/tree_explorer';
import {SLICE_TABLE} from '../widgets/sql/table_definitions';
import {extensions} from '../extensions';

export type DistributionScope = 'track' | 'all';

// The slice table shown when drilling into a flamegraph node: the standard
// (thread/process) slice view, trimmed to the columns that matter here. `id`
// keeps its slice reference so rows stay clickable to jump to the slice on the
// timeline; `utid`/`upid` stay clickable to their thread/process. `category`
// and `arg_set_id` are dropped as noise (args remain reachable via the slice).
const DRILL_SLICE_TABLE = {
  ...SLICE_TABLE,
  columns: SLICE_TABLE.columns.filter(
    (c) => c.column !== 'category' && c.column !== 'arg_set_id',
  ),
};

export function sliceDistributionCellRenderers(
  trace: Trace,
): Record<string, (value: Row[string]) => m.Children> {
  return {
    ts: (value) =>
      typeof value === 'bigint'
        ? m(Timestamp, {trace, ts: Time.fromRaw(value)})
        : String(value ?? ''),
    dur: (value) =>
      typeof value === 'bigint'
        ? m(DurationWidget, {trace, dur: value})
        : String(value ?? ''),
  };
}

const SLICE_DISTRIBUTION_SCHEMA = {
  id: NUM,
  name: STR_NULL,
  dur: LONG,
  ts: LONG,
};

export function findSliceTrackDataset(
  trace: Trace,
  trackId: number,
): Dataset | undefined {
  const track = trace.tracks.findTrack((t) =>
    t.tags?.trackIds?.includes(trackId),
  );
  const dataset = track?.renderer.getDataset?.();
  if (dataset === undefined || !dataset.implements(SLICE_DISTRIBUTION_SCHEMA)) {
    return undefined;
  }
  return dataset;
}

// Mirrors the dataset-union approach used by trace search (core/dataset_search.ts):
// the "whole trace" scope is the union of what each track exposes rather than a
// raw query against the slice table.
export function findWholeTraceSliceDataset(trace: Trace): Dataset | undefined {
  const datasets: Dataset[] = [];
  for (const track of trace.tracks.getAllTracks()) {
    const dataset = track.renderer.getDataset?.();
    if (dataset?.implements(SLICE_DISTRIBUTION_SCHEMA)) {
      datasets.push(dataset);
    }
  }
  if (datasets.length === 0) return undefined;
  if (datasets.length === 1) return datasets[0];
  return UnionDataset.create(datasets);
}

// The flamegraph roots at, at most, this many matched slices (the longest by
// value). Descendant expansion is per-root, so an unbounded root set over a
// common slice name walks a large fraction of the trace and never settles;
// capping keeps the aggregation bounded and interactive. Brushing a narrow
// range on the histogram is the way to look past the cap at a specific subset.
export const SLICE_FLAMEGRAPH_MAX_ROOTS = 512;

// Thread-scoped tracks that record what a thread was executing as a *separate*
// track from the one the matched slices live on: kernel function graph tracing
// (ftrace funcgraph_entry/exit) and ART method tracing. Their slices describe
// the inside of the matched slices even though they are not descendants of
// them in the slice table, so they are overlaid onto the matched region below.
const OVERLAY_TRACK_TYPES = "('thread_funcgraph', 'art_method_tracing')";

// Must be run before the node-set query: it uses `_interval_intersect`.
export const SLICE_FLAMEGRAPH_DEPENDENCY_SQL =
  'include perfetto module intervals.intersect;';

// Builds the node set feeding the distribution panel's slice flamegraph: the
// slices currently matched by the panel (its materialized, name/scope-filtered
// source table, narrowed to the brushed value (duration) range) as the
// flamegraph roots, plus everything that ran inside them. Roots have their
// parent_id nulled so they anchor the tree; the flamegraph then shows where the
// time inside the matched slices goes, aggregated across every matched
// instance. Works for any slices, since it uses the slice hierarchy rather than
// captured call stacks.
//
// "Everything that ran inside them" is two things:
//  - the matched slices' descendants on their own track, and
//  - function-tracing slices (funcgraph/ART method tracing) recorded on another
//    track of the same thread. Those are not descendants in the slice table, so
//    they are intersected with the matched region and chopped to it: a thread's
//    funcgraph track is a full call tree of the thread, and only the part that
//    falls inside a matched slice belongs in this flamegraph.
//
// The root set is ordered by value (duration) and capped at
// SLICE_FLAMEGRAPH_MAX_ROOTS so the per-root expansion stays bounded and the
// query is fast regardless of how common the slice name is.
export function buildSliceFlamegraphNodesSql(ctx: {
  readonly sourceTable: string;
  readonly idColumn: string;
  readonly valueColumn: string;
  readonly brush?: {readonly start: number; readonly end: number};
}): string {
  const {sourceTable, idColumn, valueColumn, brush} = ctx;
  const range =
    brush === undefined
      ? ''
      : ` AND src.${valueColumn} BETWEEN ${brush.start} AND ${brush.end}`;
  return `
    WITH
    _sfg_matching AS (
      SELECT src.${idColumn} AS id
      FROM ${sourceTable} src
      WHERE src.${idColumn} IS NOT NULL${range}
      ORDER BY src.${valueColumn} DESC
      LIMIT ${SLICE_FLAMEGRAPH_MAX_ROOTS}
    ),
    -- The matched slices and their descendants on the same track.
    _sfg_own AS MATERIALIZED (
      SELECT s.id, s.ts, s.dur, s.name, NULL AS parent_id, s.track_id
      FROM slice s
      JOIN _sfg_matching USING (id)
      UNION
      SELECT d.id, d.ts, d.dur, d.name, d.parent_id, d.track_id
      FROM _sfg_matching AS m, descendant_slice(m.id) AS d
      WHERE d.id NOT IN (SELECT id FROM _sfg_matching)
    ),
    -- Restricted to thread tracks: only there can another track describe the
    -- same execution. Non-thread (e.g. async) slices simply get no overlay.
    _sfg_own_thread AS MATERIALIZED (
      SELECT o.id, o.ts, o.dur, o.parent_id, tt.utid
      FROM _sfg_own o
      JOIN thread_track tt ON tt.id = o.track_id
      WHERE o.dur > 0 AND o.ts >= 0
    ),
    -- Each non-root node paired with its siblings' and parent's boundaries.
    -- The parent's extent is read from the slice table (an id lookup) rather
    -- than by joining the node set to itself, which would be quadratic; the IN
    -- guard keeps out nodes whose parent was dropped above (e.g. a parent that
    -- is still unfinished at the end of the trace), which would otherwise
    -- produce windows owned by a node that is not part of the tree.
    _sfg_nested AS MATERIALIZED (
      SELECT
        o.id, o.ts, o.dur, o.parent_id, o.utid,
        p.ts AS parent_ts,
        p.ts + p.dur AS parent_end,
        LAG(o.ts + o.dur) OVER _sfg_sib AS prev_sibling_end,
        LEAD(o.ts) OVER _sfg_sib AS next_sibling_ts
      FROM _sfg_own_thread o
      JOIN slice p ON p.id = o.parent_id
      WHERE o.parent_id IN (SELECT id FROM _sfg_own_thread)
      WINDOW _sfg_sib AS (PARTITION BY o.parent_id ORDER BY o.ts)
    ),
    -- The self-time windows of the matched region: the parts of a matched
    -- slice (or of one of its descendants) that none of its children covers.
    -- They are disjoint, so an overlay slice chopped against them lands wholly
    -- inside exactly one node of the tree and can never double count.
    --
    -- A window is identified by an id derived from a slice id: 2*node for a
    -- window that runs to the end of that node, and 2*child+1 for the one that
    -- runs up to the start of that child (whose owner is the child's parent).
    -- Keeping the owner in the id itself avoids joining the intersection back
    -- to this list, which is what keeps the query fast.
    _sfg_window AS (
      SELECT * FROM (
        -- Up to each child, from the parent's start or the previous sibling.
        SELECT
          n.id * 2 + 1 AS id,
          n.utid,
          IFNULL(n.prev_sibling_end, n.parent_ts) AS ts,
          n.ts - IFNULL(n.prev_sibling_end, n.parent_ts) AS dur
        FROM _sfg_nested n
        UNION ALL
        -- From the last child to the end of its parent.
        SELECT n.parent_id * 2, n.utid, n.ts + n.dur, n.parent_end - n.ts - n.dur
        FROM _sfg_nested n
        WHERE n.next_sibling_ts IS NULL
        UNION ALL
        -- Nodes without children are one window.
        SELECT o.id * 2, o.utid, o.ts, o.dur
        FROM _sfg_own_thread o
        WHERE o.id NOT IN (
          SELECT parent_id FROM _sfg_own_thread WHERE parent_id IS NOT NULL
        )
      )
      WHERE dur > 0
    ),
    -- Function-tracing slices of the same threads, pre-filtered to the span the
    -- matched slices cover so the intersection below stays proportional to the
    -- region rather than to the whole trace.
    _sfg_overlay AS (
      SELECT f.id, f.ts, f.dur, t.utid
      FROM (
        SELECT utid, MIN(ts) AS min_ts, MAX(ts + dur) AS max_end
        FROM _sfg_own_thread
        GROUP BY utid
      ) span
      JOIN thread_track t
        ON t.utid = span.utid
        AND t.type IN ${OVERLAY_TRACK_TYPES}
        AND t.id NOT IN (SELECT track_id FROM _sfg_own)
      JOIN slice f ON f.track_id = t.id
      WHERE f.dur > 0 AND f.ts >= 0
        AND f.ts < span.max_end AND f.ts + f.dur > span.min_ts
    ),
    -- Chop the overlay slices against those windows. An overlay slice that
    -- straddles a window boundary yields several pieces; the longest one is
    -- kept, so each slice contributes at most one node and ids stay unique.
    _sfg_chopped AS (
      SELECT
        x.id_1 AS id,
        x.ts,
        x.dur,
        IIF(
          x.id_0 % 2 = 0,
          x.id_0 / 2,
          (SELECT p.parent_id FROM slice p WHERE p.id = x.id_0 / 2)
        ) AS window_owner_id,
        ROW_NUMBER() OVER (PARTITION BY x.id_1 ORDER BY x.dur DESC) AS piece_rank
      FROM _interval_intersect!((_sfg_window, _sfg_overlay), (utid)) x
    ),
    _sfg_kept AS MATERIALIZED (SELECT * FROM _sfg_chopped WHERE piece_rank = 1)
    SELECT id, ts, dur, name, parent_id FROM _sfg_own
    UNION ALL
    -- An overlay slice keeps its own parent when that parent's kept piece still
    -- contains it; otherwise it hangs off the matched slice (or descendant)
    -- whose window it landed in.
    SELECT
      k.id,
      k.ts,
      k.dur,
      f.name,
      IIF(
        EXISTS(
          SELECT 1 FROM _sfg_kept p
          WHERE p.id = f.parent_id
            AND p.ts <= k.ts
            AND p.ts + p.dur >= k.ts + k.dur
        ),
        f.parent_id,
        k.window_owner_id
      ) AS parent_id
    FROM _sfg_kept k
    JOIN slice f ON f.id = k.id
  `;
}

// Mirrors the flamegraph's node naming so a NULL/empty slice name (shown as
// "unknown" in the flamegraph) matches the corresponding path segment.
function sliceNameMatches(alias: string, name: string): string {
  return (
    `IIF(IFNULL(${alias}.name, '') = '', 'unknown', ${alias}.name) = ` +
    sqlValueToSqliteString(name)
  );
}

// The id sub-select a flamegraph-node drill targets. When the clicked node's
// tree `path` is known (root->node names) it targets exactly that box -- the
// region slices whose own root->node name path matches -- so the drilled count
// equals the count shown on the node. A name appearing under several parents
// forms several boxes, and each drills to only its own slices.
//
// The path is matched by walking the node set one parent_id edge per segment
// (no recursion): a region root, then a direct child per name down the path.
// The walk goes over the node set rather than the slice table because the two
// differ: an overlaid function-tracing slice hangs off the matched slice it ran
// inside, which is not its parent in the slice table. Without a path (e.g. a
// non-flamegraph caller) it falls back to matching by name across the region.
function sliceFlamegraphDrillIds(
  nodesSql: string,
  node: {readonly name: string},
  path?: ReadonlyArray<string>,
): string {
  if (path === undefined || path.length === 0) {
    return `SELECT id FROM (${nodesSql}) WHERE name = ${sqlValueToSqliteString(node.name)}`;
  }
  const last = path.length - 1;
  const joins = path
    .slice(1)
    .map(
      (name, i) =>
        `JOIN _nodes s${i + 1} ON s${i + 1}.parent_id = s${i}.id AND ` +
        sliceNameMatches(`s${i + 1}`, name),
    )
    .join('\n    ');
  // MATERIALIZED: the node set is walked once per path segment, and rebuilding
  // it each time would re-run the whole region expansion.
  return `
    WITH _nodes AS MATERIALIZED (${nodesSql})
    SELECT s${last}.id AS id
    FROM _nodes s0
    ${joins}
    WHERE s0.parent_id IS NULL AND ${sliceNameMatches('s0', path[0])}
  `;
}

// Drill-down actions offered on each flamegraph node. `nodesSql` is the
// region's node set (roots + descendants for the brushed range), built from a
// stable source query so the new tab/track outlives the panel that spawned it.
// Clicking a node opens exactly that box's slices (its tree position, not every
// same-named slice) as a plain sortable/filterable slice table -- a leaf, so
// there is no confusing recursive nesting of flamegraphs -- or drops them onto
// the timeline as a debug track. Ids in the table stay clickable to jump to the
// timeline.
export function buildSliceFlamegraphNodeActions(
  trace: Trace,
): (ctx: {
  readonly nodesSql: string;
}) => ReadonlyArray<TreeExplorerOptionalAction> {
  return ({nodesSql}) => {
    return [
      {
        name: 'Open matching slices',
        icon: 'table_view',
        category: 'DRILL',
        description:
          'Open the slices under this node (its position in the tree) in a ' +
          'sortable, filterable table. Click an id there to jump to the ' +
          'timeline.',
        execute: ({node, path}) => {
          if (node === undefined) return;
          const ids = sliceFlamegraphDrillIds(nodesSql, node, path);
          extensions.addLegacySqlTableTab(trace, {
            table: DRILL_SLICE_TABLE,
            filters: [
              {columns: ['id'], op: (cols) => `${cols[0]} IN (${ids})`},
            ],
          });
        },
      },
      {
        name: 'Add debug track',
        icon: 'add_chart',
        category: 'DRILL',
        description:
          'Add the slices under this node to the timeline as a debug track.',
        execute: ({node, path}) => {
          if (node === undefined) return;
          // Carry the source slice id (and table_name='slice') so a slice
          // selected on the debug track links back to the real slice in its
          // details panel (debug tracks otherwise mint their own ids).
          const ids = sliceFlamegraphDrillIds(nodesSql, node, path);
          extensions.addDebugSliceTrack({
            trace,
            data: {
              sqlSource:
                `SELECT s.id, s.ts, s.dur, s.name, 'slice' AS table_name ` +
                `FROM slice s WHERE s.id IN (${ids})`,
            },
            title: `${node.name} (in region)`,
          });
        },
      },
    ];
  };
}

export function sliceDistributionConfig(
  trace: Trace,
  sliceName: string,
  dataset: Dataset,
  scope: DistributionScope,
): Omit<DistributionPanelAttrs, 'trace'> {
  const scopeLabel = scope === 'track' ? 'this track' : 'across trace';
  return {
    title: `${sliceName} (${scopeLabel})`,
    dataset,
    filter: {col: 'name', eq: sliceName},
    valueColumn: 'dur',
    idColumn: 'id',
    sqlTable: 'slice',
    displayColumns: ['ts', 'dur'],
    cellRenderers: sliceDistributionCellRenderers(trace),
    flamegraphNodesSql: buildSliceFlamegraphNodesSql,
    flamegraphDependencySql: SLICE_FLAMEGRAPH_DEPENDENCY_SQL,
    flamegraphNodeActions: buildSliceFlamegraphNodeActions(trace),
  };
}

function resolveSliceDataset(
  trace: Trace,
  trackId: number,
  scope: DistributionScope,
): Dataset | undefined {
  return scope === 'track'
    ? findSliceTrackDataset(trace, trackId)
    : findWholeTraceSliceDataset(trace);
}

function openSliceDistribution(
  trace: Trace,
  sliceName: string,
  trackId: number,
  scope: DistributionScope,
): void {
  const dataset = resolveSliceDataset(trace, trackId, scope);
  if (dataset === undefined) return;
  openDistributionTab(
    trace,
    sliceDistributionConfig(trace, sliceName, dataset, scope),
  );
}

function renderMatchingSlicesMenu(
  trace: Trace,
  slice: SliceDetails,
): m.Children {
  const sliceName = slice.name;
  if (sliceName === undefined) {
    return m(MenuItem, {
      label: 'Slices with the same name',
      disabled: true,
    });
  }
  const item = (label: string, scope: DistributionScope) =>
    m(MenuItem, {
      label,
      onclick: () =>
        openSliceDistribution(trace, sliceName, slice.trackId, scope),
    });
  return [
    item('Slices with the same name (this track)', 'track'),
    item('Slices with the same name (across trace)', 'all'),
  ];
}

export function renderDetails(
  trace: Trace,
  slice: SliceDetails,
  durationBreakdown?: BreakdownByThreadState,
) {
  return m(
    Section,
    {title: 'Details'},
    m(
      Tree,
      m(TreeNode, {
        left: 'Name',
        right: m(
          PopupMenu,
          {
            trigger: m(Anchor, slice.name),
          },
          renderMatchingSlicesMenu(trace, slice),
        ),
      }),
      m(TreeNode, {
        left: 'Category',
        right:
          !slice.category || slice.category === '[NULL]'
            ? 'N/A'
            : slice.category,
      }),
      m(TreeNode, {
        left: 'Start time',
        right: m(Timestamp, {trace, ts: slice.ts}),
      }),
      exists(slice.absTime) &&
        m(TreeNode, {left: 'Absolute Time', right: slice.absTime}),
      m(
        TreeNode,
        {
          left: 'Duration',
          right: m(DurationWidget, {trace, dur: slice.dur}),
        },
        exists(durationBreakdown) &&
          slice.dur > 0 &&
          m(BreakdownByThreadStateTreeNode, {
            trace,
            data: durationBreakdown,
            dur: slice.dur,
          }),
      ),
      renderThreadDuration(trace, slice),
      slice.thread &&
        m(TreeNode, {
          left: 'Thread',
          right: renderThreadRef(trace, slice.thread),
        }),
      slice.process &&
        m(TreeNode, {
          left: 'Process',
          right: renderProcessRef(trace, slice.process),
        }),
      slice.process &&
        exists(slice.process.uid) &&
        m(TreeNode, {
          left: 'User ID',
          right: slice.process.uid,
        }),
      slice.process &&
        slice.process.packageName &&
        m(TreeNode, {
          left: 'Package name',
          right: slice.process.packageName,
        }),
      slice.process &&
        exists(slice.process.versionCode) &&
        m(TreeNode, {
          left: 'Version code',
          right: slice.process.versionCode,
        }),
      m(TreeNode, {
        left: 'SQL ID',
        right: m(SqlRef, {table: 'slice', id: slice.id}),
      }),
    ),
  );
}

function renderThreadDuration(trace: Trace, sliceInfo: SliceDetails) {
  if (exists(sliceInfo.threadTs) && exists(sliceInfo.threadDur)) {
    // If we have valid thread duration, also display a percentage of
    // |threadDur| compared to |dur|.
    const ratio = BigintMath.ratio(sliceInfo.threadDur, sliceInfo.dur);
    const threadDurFractionSuffix =
      sliceInfo.threadDur === -1n ? '' : ` (${(ratio * 100).toFixed(2)}%)`;
    return m(TreeNode, {
      left: 'Thread duration',
      right: [
        m(DurationWidget, {trace, dur: sliceInfo.threadDur}),
        threadDurFractionSuffix,
      ],
    });
  } else {
    return undefined;
  }
}
