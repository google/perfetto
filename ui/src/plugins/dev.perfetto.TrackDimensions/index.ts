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

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {TrackNode} from '../../public/workspace';
import {
  type Dimension,
  dimensionValue,
  formatDimensionLabels,
  visibleDimensionNames,
} from '../../public/dimensions';
import {
  NUM,
  NUM_NULL,
  type QueryResult,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import TrackEventPlugin from '../dev.perfetto.TrackEvent';

// The subtitle item id used for dimension labels. Distinct from the ids used by
// other contributors (e.g. Chrome process labels) so the two compose instead of
// overwriting each other.
const SUBTITLE_ITEM_ID = 'dimensions';

// A dimension plus the id of the thing it applies to: a track id, a upid or a
// utid depending on which relation it was read from.
interface KeyedDimension {
  readonly key: number;
  readonly dimension: Dimension;
}

/**
 * Surfaces track dimensions in the timeline.
 *
 * Dimensions are a trace processor concept first: this plugin implements the
 * shared presentation half of it, i.e. the collapse rule ("only show a
 * dimension when it tells the peers apart") and the subtitle labels which come
 * out of it. Well known dimensions which already
 * have specialized presentation (GPU hierarchy, machine name suffixes) are
 * skipped: see public/dimensions.ts.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TrackDimensions';
  static readonly dependencies = [ProcessThreadGroupsPlugin, TrackEventPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Wait for the trace to be ready: every plugin has had a chance to build
    // its part of the workspace by then, which is what we annotate.
    ctx.onTraceReady.addListener(async () => {
      const [trackDims, processDims, threadDims] = await Promise.all([
        queryDimensions(ctx, 'track_dimension', 'track_id'),
        queryDimensions(ctx, 'process_dimension', 'upid'),
        queryDimensions(ctx, 'thread_dimension', 'utid'),
      ]);
      if (!trackDims.length && !processDims.length && !threadDims.length) {
        return;
      }
      const cohorts = await queryCohortSizes(ctx);

      // The collapse rule is global: a dimension disambiguates nothing when
      // every one of its peers carries it with the same value.
      const absentOnSomePeers = new Set<string>();
      addNamesMissingFromSomePeer(trackDims, cohorts.tracks, absentOnSomePeers);
      addNamesMissingFromSomePeer(
        processDims,
        cohorts.processes,
        absentOnSomePeers,
      );
      addNamesMissingFromSomePeer(
        threadDims,
        cohorts.threads,
        absentOnSomePeers,
      );
      const visible = visibleDimensionNames(
        [
          ...trackDims.map((d) => d.dimension),
          ...processDims.map((d) => d.dimension),
          ...threadDims.map((d) => d.dimension),
        ],
        absentOnSomePeers,
      );
      if (visible.size === 0) return;

      const byNode = new Map<TrackNode, Dimension[]>();
      const add = (node: TrackNode | undefined, dimension: Dimension) => {
        if (node === undefined) return;
        if (!visible.has(dimension.name)) return;
        const dims = byNode.get(node);
        if (dims === undefined) {
          byNode.set(node, [dimension]);
        } else if (!dims.some((d) => d.name === dimension.name)) {
          dims.push(dimension);
        }
      };

      // Process and thread groups are keyed by upid/utid rather than by track.
      for (const {key, dimension} of processDims) {
        add(
          ctx.plugins
            .getPlugin(ProcessThreadGroupsPlugin)
            .getGroupForProcess(key),
          dimension,
        );
      }
      for (const {key, dimension} of threadDims) {
        add(
          ctx.plugins
            .getPlugin(ProcessThreadGroupsPlugin)
            .getGroupForThread(key),
          dimension,
        );
      }

      // Everything else is keyed by trace processor track id, which we map back
      // to workspace nodes through the tracks' `trackIds` tag.
      const nodesByTrackId = getNodesByTrackId(ctx);
      for (const {key, dimension} of trackDims) {
        add(nodesByTrackId.get(key), dimension);
      }

      for (const [node, dims] of byNode) {
        // A dimension is inherited by everything below the node which declared
        // it, so repeating the same label on every descendant is just noise:
        // only label a node when it says something its ancestors don't.
        const newDims = dims
          .filter((d) => !isShownByAncestor(node, d, byNode))
          .sort((a, b) => a.name.localeCompare(b.name));
        node.setSubtitleItem(SUBTITLE_ITEM_ID, formatDimensionLabels(newDims));
      }
    });
  }
}

// Records the dimensions which only some of the tracks/processes/threads in
// |dims| carry: those still disambiguate, even when every value is the same.
function addNamesMissingFromSomePeer(
  dims: ReadonlyArray<KeyedDimension>,
  cohortSize: number,
  out: Set<string>,
): void {
  const keysByName = new Map<string, Set<number>>();
  for (const {key, dimension} of dims) {
    const keys = keysByName.get(dimension.name) ?? new Set<number>();
    keys.add(key);
    keysByName.set(dimension.name, keys);
  }
  for (const [name, keys] of keysByName) {
    if (keys.size < cohortSize) out.add(name);
  }
}

function isShownByAncestor(
  node: TrackNode,
  dimension: Dimension,
  byNode: ReadonlyMap<TrackNode, ReadonlyArray<Dimension>>,
): boolean {
  const value = dimensionValue(dimension);
  for (let n = node.parent; n !== undefined; n = n.parent) {
    const dims = byNode.get(n);
    if (
      dims?.some(
        (d) => d.name === dimension.name && dimensionValue(d) === value,
      )
    ) {
      return true;
    }
  }
  return false;
}

function getNodesByTrackId(ctx: Trace): Map<number, TrackNode> {
  const result = new Map<number, TrackNode>();
  for (const track of ctx.tracks.getAllTracks()) {
    const node = ctx.defaultWorkspace.getTrackByUri(track.uri);
    if (node === undefined) continue;
    for (const trackId of track.tags?.trackIds ?? []) {
      result.set(trackId, node);
    }
  }
  return result;
}

// Note: this deliberately asks only for custom dimensions. Every well known
// dimension is currently presented elsewhere in the UI (see the list in
// public/dimensions.ts), and skipping them means traces without custom
// dimensions - i.e. almost all of them today - do no work here at all. Drop
// the filter when a well known dimension starts being labelled by this pass.
async function queryDimensions(
  ctx: Trace,
  table: string,
  keyColumn: string,
): Promise<ReadonlyArray<KeyedDimension>> {
  const res = await ctx.engine.query(`
    SELECT ${keyColumn} AS key, name, int_value, string_value, display_name
    FROM ${table}
    WHERE is_well_known = 0
    ORDER BY ${keyColumn}, name
  `);
  return readDimensions(res);
}

async function queryCohortSizes(ctx: Trace) {
  const res = await ctx.engine.query(`
    SELECT
      (SELECT count(*) FROM track) AS tracks,
      (SELECT count(*) FROM process) AS processes,
      (SELECT count(*) FROM thread) AS threads
  `);
  return res.firstRow({tracks: NUM, processes: NUM, threads: NUM});
}

function readDimensions(res: QueryResult): ReadonlyArray<KeyedDimension> {
  const it = res.iter({
    key: NUM,
    name: STR,
    int_value: NUM_NULL,
    string_value: STR_NULL,
    display_name: STR_NULL,
  });
  const out: KeyedDimension[] = [];
  for (; it.valid(); it.next()) {
    out.push({
      key: it.key,
      dimension: {
        name: it.name,
        intValue: it.int_value,
        stringValue: it.string_value,
        displayName: it.display_name,
      },
    });
  }
  return out;
}
