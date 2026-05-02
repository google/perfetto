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

import {SliceTrack} from '../../components/tracks/slice_track';
import {getColorForSlice} from '../../components/colorizer';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {showModal} from '../../widgets/modal';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import {DebugSliceTrackDetailsPanel} from '../../components/tracks/debug_slice_track_details_panel';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

type ChildThread = {
  utid: number;
  name: string | null;
  rowIds: number[];
  firstTs: bigint;
};

const PER_TRACK_SLICE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  name: STR,
  // raw_* expose the thread_state row backing each slice so the
  // details panel can jump to it.
  raw_id: NUM,
  raw_table_name: STR,
  raw_utid: NUM,
};

// Pins one critical-path drill-down tree for a (root utid, window).
// Each instance owns a per-invocation set of perfetto tables and a
// track URI namespace, so concurrent pins don't collide.
//
// A track at depth N for thread U shows full coverage of U's
// chain: own frames (depth N) named after U, and every deeper
// frame attributed to its depth-(N+1) ancestor in the chain
// (named after that ancestor's thread = the immediate blocker).
// The depth-N and depth-(N+1) ancestor maps are built once via
// `_graph_aggregating_scan!` over a dense renumbering of chain
// nodes (graphs.scan requires dense ids).
export class CriticalPathTreePin {
  private framesTable = '';
  private denseNodesTable = '';
  private denseEdgesTable = '';
  private hasChildrenTable = '';
  private baseUri = '';
  private readonly builtAncDepths = new Set<number>();
  private nextTrackId = 0;

  constructor(
    private readonly ctx: Trace,
    private readonly rootUtid: number,
    private readonly rootName: string,
    private readonly windowTs: bigint,
    private readonly windowDur: bigint,
  ) {}

  async pin(): Promise<void> {
    await this.ctx.engine.query(
      `INCLUDE PERFETTO MODULE sched.thread_executing_span;`,
    );
    await this.ctx.engine.query(`INCLUDE PERFETTO MODULE graphs.scan;`);
    await this.createFrames();
    if (await this.isFramesEmpty()) {
      return showModal({
        title: 'Critical path: no chain found',
        content:
          'No wakeup-graph attribution for this thread over this ' +
          'window. Trace may be missing sched_switch / sched_waking, ' +
          'or this thread never slept with a recorded waker.',
      });
    }
    await this.createDenseNodes();
    await this.createDenseEdges();
    await this.createHasChildren();

    const rootAnchors = await this.fetchRootAnchors();
    if (rootAnchors.length === 0) {
      return showModal({
        title: 'Critical path: root has no on-CPU contribution',
        content:
          'The root thread does not appear at depth 0 in the chain ' +
          'frames for this window — the chain may be entirely from ' +
          'wakers with no self-runs in this range.',
      });
    }

    const rootNode = await this.makeRootNode(rootAnchors);
    this.ctx.currentWorkspace.pinnedTracksNode.addChildLast(rootNode);
  }

  // One row per chain frame: each `(depth, utid)` frame in the
  // critical path of `rootUtid` over the window.
  private async createFrames(): Promise<void> {
    const t = await createPerfettoTable({
      engine: this.ctx.engine,
      as: `
        SELECT
          row_number() OVER (ORDER BY cr.depth, cr.ts) AS id,
          cr.root_id AS root_id,
          cr.id AS node_id,
          cr.parent_id AS parent_node_id,
          cr.depth AS depth,
          cr.ts AS ts,
          ifnull(CAST(cr.dur AS INT), -1) AS dur,
          cr.utid AS utid,
          thread.name AS thread_name
        FROM _critical_path_with_depth_by_intervals!(
               (SELECT ${this.rootUtid} AS utid,
                       ${this.windowTs.toString()} AS ts,
                       ${this.windowDur.toString()} AS dur),
               _wakeup_graph) AS cr
        JOIN thread USING (utid)
      `,
    });
    this.framesTable = t.name;
    this.baseUri = `dev.perfetto.CriticalPathTree.${this.framesTable}`;
    await this.ctx.engine.query(`
      CREATE PERFETTO INDEX ${this.framesTable}_child_idx
        ON ${this.framesTable}(depth, root_id, parent_node_id)
    `);
  }

  private async isFramesEmpty(): Promise<boolean> {
    const r = await this.ctx.engine.query(
      `SELECT COUNT(*) AS n FROM ${this.framesTable}`,
    );
    return r.firstRow({n: NUM}).n === 0;
  }

  // Dense [1..N] renumbering of (root_id, node_id) — graphs.scan
  // requires dense node ids; wakeup-graph node_ids are sparse.
  private async createDenseNodes(): Promise<void> {
    const t = await createPerfettoTable({
      engine: this.ctx.engine,
      as: `
        SELECT row_number() OVER (ORDER BY root_id, node_id) AS dense_id,
               root_id, node_id,
               MAX(parent_node_id) AS parent_node_id,
               MAX(depth) AS depth, MAX(utid) AS utid
        FROM ${this.framesTable}
        GROUP BY root_id, node_id
      `,
    });
    this.denseNodesTable = t.name;
    await this.ctx.engine.query(`
      CREATE PERFETTO INDEX ${this.denseNodesTable}_lookup
        ON ${this.denseNodesTable}(root_id, node_id)
    `);
    await this.ctx.engine.query(`
      CREATE PERFETTO INDEX ${this.denseNodesTable}_id
        ON ${this.denseNodesTable}(dense_id)
    `);
  }

  // (parent dense_id -> child dense_id) within each chain. The
  // with-depth macro emits parent_node_id = node_id for depth-0
  // rows; that self-loop must be filtered, graphs.scan requires a
  // DAG.
  private async createDenseEdges(): Promise<void> {
    const t = await createPerfettoTable({
      engine: this.ctx.engine,
      as: `
        SELECT p.dense_id AS source_node_id, c.dense_id AS dest_node_id
        FROM ${this.denseNodesTable} c
        JOIN ${this.denseNodesTable} p
          ON p.root_id = c.root_id AND p.node_id = c.parent_node_id
        WHERE c.parent_node_id IS NOT NULL
          AND c.parent_node_id <> c.node_id
      `,
    });
    this.denseEdgesTable = t.name;
  }

  // Frame ids that have at least one direct child; used to suppress
  // the expand arrow on terminal tracks.
  private async createHasChildren(): Promise<void> {
    const t = await createPerfettoTable({
      engine: this.ctx.engine,
      as: `
        SELECT DISTINCT p.id AS row_id
        FROM ${this.framesTable} p
        JOIN ${this.framesTable} c
          ON c.root_id = p.root_id
         AND c.parent_node_id = p.node_id
         AND c.depth = p.depth + 1
      `,
    });
    this.hasChildrenTable = t.name;
  }

  private async anyAnchorHasChildren(
    rowIds: ReadonlyArray<number>,
  ): Promise<boolean> {
    if (rowIds.length === 0) return false;
    const r = await this.ctx.engine.query(`
      SELECT COUNT(*) AS n FROM ${this.hasChildrenTable}
      WHERE row_id IN (${rowIds.join(',')})
    `);
    return r.firstRow({n: NUM}).n > 0;
  }

  // Per-depth ancestor map (dense_id -> depth-D ancestor),
  // memoised across sibling tracks.
  private ancTableForDepth(d: number): string {
    return `${this.framesTable}_anc_d${d}`;
  }

  private async ensureAncTable(d: number): Promise<void> {
    if (this.builtAncDepths.has(d)) return;
    this.builtAncDepths.add(d);
    const t = this.ancTableForDepth(d);
    await this.ctx.engine.query(`
      CREATE PERFETTO TABLE ${t} AS
      SELECT id AS dense_id, anc_id
      FROM _graph_aggregating_scan!(
        ${this.denseEdgesTable},
        (SELECT dense_id AS id, dense_id AS anc_id
         FROM ${this.denseNodesTable} WHERE depth = ${d}),
        (anc_id),
        (SELECT id, MIN(anc_id) AS anc_id FROM $table GROUP BY id))
    `);
    await this.ctx.engine.query(`
      CREATE PERFETTO INDEX ${t}_idx ON ${t}(dense_id)
    `);
  }

  // For a track at depth N for thread U:
  //   * Frames at any depth M >= N whose depth-N ancestor is one of
  //     our anchors are in our chain.
  //   * Slice name & raw_id come from the depth-(N+1) ancestor (the
  //     immediate blocker); when M = N (own on-CPU frame) the name
  //     comes from the frame itself.
  private async registerTrack(
    depth: number,
    rowIds: ReadonlyArray<number>,
  ): Promise<string> {
    const uri = `${this.baseUri}.t${this.nextTrackId++}`;
    const anchorIdsCsv = rowIds.join(',');
    await this.ensureAncTable(depth);
    await this.ensureAncTable(depth + 1);
    const ancTable = this.ancTableForDepth(depth);
    const blkTable = this.ancTableForDepth(depth + 1);
    let materializedTableName = '';
    const renderer = await SliceTrack.createMaterialized({
      trace: this.ctx,
      uri,
      dataset: new SourceDataset({
        schema: PER_TRACK_SLICE_SCHEMA,
        src: `WITH anchors_dense AS (
                SELECT DISTINCT dn.dense_id
                FROM ${this.framesTable} f
                JOIN ${this.denseNodesTable} dn
                  ON dn.root_id = f.root_id AND dn.node_id = f.node_id
                WHERE f.id IN (${anchorIdsCsv})
              ),
              picked AS (
                SELECT
                  f.ts, f.dur, f.depth,
                  CASE WHEN f.depth = ${depth}
                       THEN f.utid ELSE blk.utid END AS slice_utid,
                  CASE WHEN f.depth = ${depth}
                       THEN f.node_id ELSE blk.node_id END AS slice_node_id
                FROM ${this.framesTable} f
                JOIN ${this.denseNodesTable} dn
                  ON dn.root_id = f.root_id AND dn.node_id = f.node_id
                JOIN ${ancTable} ad ON ad.dense_id = dn.dense_id
                JOIN anchors_dense ax ON ax.dense_id = ad.anc_id
                LEFT JOIN ${blkTable} bd ON bd.dense_id = dn.dense_id
                LEFT JOIN ${this.denseNodesTable} blk
                  ON blk.dense_id = bd.anc_id
                WHERE f.depth >= ${depth}
              )
              SELECT
                row_number() OVER (ORDER BY p.ts) AS id,
                p.ts, p.dur,
                coalesce(thread.name, 'utid ' || p.slice_utid) AS name,
                p.slice_node_id AS raw_id,
                'thread_state' AS raw_table_name,
                p.slice_utid AS raw_utid
              FROM picked p
              JOIN thread ON thread.utid = p.slice_utid`,
      }),
      colorizer: (row) => getColorForSlice(row.name),
      detailsPanel: (row) =>
        new DebugSliceTrackDetailsPanel(
          this.ctx,
          materializedTableName,
          row.id,
        ),
    });
    materializedTableName = renderer.getDataset()?.src ?? '';
    this.ctx.tracks.registerTrack({uri, renderer});
    return uri;
  }

  // Returns the depth-(N+1) child threads of the given anchors,
  // ordered by first appearance.
  private async queryChildren(
    parentAnchorRowIds: ReadonlyArray<number>,
  ): Promise<ChildThread[]> {
    if (parentAnchorRowIds.length === 0) return [];
    const res = await this.ctx.engine.query(`
      SELECT c.id AS row_id, c.utid, c.ts, c.thread_name AS tname
      FROM ${this.framesTable} p
      JOIN ${this.framesTable} c
        ON c.root_id = p.root_id
       AND c.parent_node_id = p.node_id
       AND c.depth = p.depth + 1
      WHERE p.id IN (${parentAnchorRowIds.join(',')})
      ORDER BY c.ts
    `);
    const it = res.iter({
      row_id: NUM,
      utid: NUM,
      ts: LONG,
      tname: STR_NULL,
    });
    const byUtid = new Map<number, ChildThread>();
    for (; it.valid(); it.next()) {
      let th = byUtid.get(it.utid);
      if (!th) {
        th = {utid: it.utid, name: it.tname, rowIds: [], firstTs: it.ts};
        byUtid.set(it.utid, th);
      }
      th.rowIds.push(it.row_id);
      if (it.ts < th.firstTs) th.firstTs = it.ts;
    }
    return Array.from(byUtid.values()).sort((a, b) =>
      a.firstTs < b.firstTs ? -1 : a.firstTs > b.firstTs ? 1 : 0,
    );
  }

  // Thread track at the given depth; depth-(N+1) child threads (if
  // any) are queried and minted on first expand.
  private async makeThreadTrack(
    depth: number,
    label: string,
    rowIds: ReadonlyArray<number>,
    removable = false,
  ): Promise<TrackNode> {
    const uri = await this.registerTrack(depth, rowIds);
    if (!(await this.anyAnchorHasChildren(rowIds))) {
      return new TrackNode({uri, name: label, removable});
    }
    let opened = false;
    const node = new TrackNode({
      uri,
      name: label,
      removable,
      onExpand: () => {
        if (opened) return;
        opened = true;
        void (async () => {
          const children = await this.queryChildren(rowIds);
          const childNodes = await Promise.all(
            children.map((c) =>
              this.makeThreadTrack(
                depth + 1,
                c.name ?? `<utid ${c.utid}>`,
                c.rowIds,
              ),
            ),
          );
          for (const c of [...node.children]) node.removeChild(c);
          for (const c of childNodes) node.addChildLast(c);
        })();
      },
    });
    // Empty placeholder so the parent shows an expand arrow before
    // its real children are loaded; replaced on first expand.
    node.addChildLast(new TrackNode({name: '', headless: true}));
    return node;
  }

  private async fetchRootAnchors(): Promise<number[]> {
    const r = await this.ctx.engine.query(`
      SELECT id FROM ${this.framesTable}
      WHERE depth = 0 AND utid = ${this.rootUtid}
      ORDER BY ts
    `);
    const out: number[] = [];
    for (const it = r.iter({id: NUM}); it.valid(); it.next()) out.push(it.id);
    return out;
  }

  private async makeRootNode(rootAnchors: number[]): Promise<TrackNode> {
    return this.makeThreadTrack(
      0,
      this.rootName,
      rootAnchors,
      /* removable=*/ true,
    );
  }
}
