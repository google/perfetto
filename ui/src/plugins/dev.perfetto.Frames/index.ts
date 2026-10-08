// Copyright (C) 2021 The Android Open Source Project
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

import type {App} from '../../public/app';
import {getOrCreate} from '../../base/utils';
import {createAggregationTab} from '../../components/aggregation_adapter';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode} from '../../public/workspace';
import {NUM, STR} from '../../trace_processor/query_result';
import type {QueryResult} from '../../trace_processor/query_result';
import type {Engine} from '../../trace_processor/engine';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {createActualFramesTrack} from './actual_frames_track';
import {
  createExpectedFramesTrack,
  EXPECTED_FRAMES_BY_LAYER,
} from './expected_frames_track';
import {
  ACTUAL_FRAMES_SLICE_TRACK_KIND,
  FrameSelectionAggregator,
} from './frame_selection_aggregator';
import type {Setting} from '../../public/settings';
import {z} from 'zod';

const EXPECTED_TIMELINE_TRACK_NAME = 'Expected Timeline';
const ACTUAL_TIMELINE_TRACK_NAME = 'Actual Timeline';
const ACTUAL_TIMELINE_EXPERIMENTAL_TRACK_NAME =
  'Actual Timeline (Experimental)';

// The frame timelines sit at the very top of the process group.
const TIMELINE_SORT_ORDER = -50;
const EXPERIMENTAL_TIMELINE_SORT_ORDER = -49;

// Build a standardized URI for a frames track
function makeUri(upid: number, kind: string) {
  return `/process_${upid}/${kind}`;
}

// Build a standardized URI for a frames track scoped to a single layer.
function makeLayerUri(upid: number, kind: string, layerName: string) {
  return `${makeUri(upid, kind)}/${encodeURIComponent(layerName)}`;
}

/**
 * Returns the URIs of the process-level frame timeline tracks of a process:
 * actual, actual (experimental) and expected, in this order. The per-layer
 * tracks are not included.
 *
 * Use this rather than matching on track names, as the per-layer tracks share
 * the names of the process-level ones. Not all of these tracks necessarily
 * exist.
 */
export function getProcessFrameTimelineUris(upid: number): string[] {
  return [
    makeUri(upid, 'actual_frames'),
    makeUri(upid, 'actual_frames_experimental'),
    makeUri(upid, 'expected_frames'),
  ];
}

export default class FramesPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Frames';
  static readonly dependencies = [ProcessThreadGroupsPlugin];
  static showExperimentalJankClassification: Setting<boolean>;

  static onActivate(app: App): void {
    FramesPlugin.showExperimentalJankClassification = app.settings.register({
      id: `${FramesPlugin.id}#showExperimentalJankClassification`,
      name: 'show experimental jank classification track (alpha)',
      description: 'Use alternative method to classify jank. Not recommented.',
      schema: z.boolean(),
      defaultValue: false,
      requiresReload: true,
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    await Promise.all([this.addExpectedFrames(ctx), this.addActualFrames(ctx)]);
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new FrameSelectionAggregator(ctx), 10),
    );
  }

  private async addExpectedFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const result = await engine.query(`
      with summary as (
        select
          pt.upid,
          group_concat(id) AS track_ids,
          count() AS track_count
        from process_track pt
        join _slice_track_summary USING (id)
        where pt.type = 'android_expected_frame_timeline'
        group by pt.upid
      )
      select
        t.upid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from summary t
    `);

    const it = result.iter({
      upid: NUM,
      trackIds: STR,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;

      const uri = makeUri(upid, 'expected_frames');
      ctx.tracks.registerTrack({
        uri,
        renderer: createExpectedFramesTrack(ctx, uri, maxDepth, upid),
        tags: {
          kinds: [SLICE_TRACK_KIND],
          trackIds,
          upid,
        },
      });
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);
      const track = new TrackNode({
        uri,
        name: EXPECTED_TIMELINE_TRACK_NAME,
        sortOrder: TIMELINE_SORT_ORDER,
      });
      group?.addChildInOrder(track);
    }
  }

  private async addActualFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
    const layersByProcess = await queryFrameTracksByLayer(engine);
    const result = await engine.query(`
      with summary as (
        select
          pt.upid,
          group_concat(id) AS track_ids,
          count() AS track_count
        from process_track pt
        join _slice_track_summary USING (id)
        where pt.type = 'android_actual_frame_timeline'
        group by pt.upid
      )
      select
        t.upid,
        t.track_ids as trackIds,
        __max_layout_depth(t.track_count, t.track_ids) as maxDepth
      from summary t
    `);

    const it = result.iter({
      upid: NUM,
      trackIds: STR,
      maxDepth: NUM,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid;
      const rawTrackIds = it.trackIds;
      const trackIds = rawTrackIds.split(',').map((v) => Number(v));
      const maxDepth = it.maxDepth;
      const group = ctx.plugins
        .getPlugin(ProcessThreadGroupsPlugin)
        .getGroupForProcess(upid);

      // Standard actual frames track
      const standardUri = makeUri(upid, 'actual_frames');
      ctx.tracks.registerTrack({
        uri: standardUri,
        renderer: createActualFramesTrack(
          ctx,
          standardUri,
          maxDepth,
          upid,
          false,
        ),
        tags: {
          upid,
          trackIds,
          kinds: [SLICE_TRACK_KIND, ACTUAL_FRAMES_SLICE_TRACK_KIND],
        },
      });
      const actualTimeline = new TrackNode({
        uri: standardUri,
        name: ACTUAL_TIMELINE_TRACK_NAME,
        sortOrder: TIMELINE_SORT_ORDER,
      });
      group?.addChildInOrder(actualTimeline);
      this.addLayers(ctx, actualTimeline, upid, layersByProcess.get(upid));

      // Experimental jank classification track (if enabled)
      if (FramesPlugin.showExperimentalJankClassification.get()) {
        const experimentalUri = makeUri(upid, 'actual_frames_experimental');
        ctx.tracks.registerTrack({
          uri: experimentalUri,
          renderer: createActualFramesTrack(
            ctx,
            experimentalUri,
            maxDepth,
            upid,
            true,
          ),
          tags: {
            upid,
            trackIds,
            kinds: [SLICE_TRACK_KIND],
          },
        });
        group?.addChildInOrder(
          new TrackNode({
            uri: experimentalUri,
            name: ACTUAL_TIMELINE_EXPERIMENTAL_TRACK_NAME,
            sortOrder: EXPERIMENTAL_TIMELINE_SORT_ORDER,
          }),
        );
      }
    }
  }

  // Adds the expected and actual frame timelines of each of the process'
  // layers as children of the process' 'Actual Timeline' track.
  private addLayers(
    ctx: Trace,
    actualTimeline: TrackNode,
    upid: number,
    layers: ReadonlyMap<string, LayerFrameTracks> | undefined,
  ): void {
    if (layers === undefined) return;
    const sortedLayers = Array.from(layers).sort(([a], [b]) =>
      a.localeCompare(b),
    );
    for (const [layerName, tracks] of sortedLayers) {
      actualTimeline.addChildLast(
        this.createLayerNode(ctx, upid, layerName, tracks),
      );
    }
  }

  private createLayerNode(
    ctx: Trace,
    upid: number,
    layerName: string,
    tracks: LayerFrameTracks,
  ): TrackNode {
    const layerNode = new TrackNode({name: layerName});
    if (tracks.expected !== undefined) {
      this.addLayerExpectedTrack(
        ctx,
        layerNode,
        upid,
        layerName,
        tracks.expected,
      );
    }
    if (tracks.actual !== undefined) {
      this.addLayerActualTracks(ctx, layerNode, upid, layerName, tracks.actual);
    }
    return layerNode;
  }

  // Layer tracks deliberately omit the 'trackIds' tag. That tag feeds a global
  // track id -> track index which assumes a single owner per id, and a layer
  // track only renders a subset of the slices of the process' trace processor
  // tracks. Claiming them would shadow the process level timelines when other
  // subsystems (search, flow events, ...) resolve a slice back to its track.
  private addLayerExpectedTrack(
    ctx: Trace,
    layerNode: TrackNode,
    upid: number,
    layerName: string,
    track: FrameTrackSummary,
  ): void {
    const uri = makeLayerUri(upid, 'expected_frames', layerName);
    ctx.tracks.registerTrack({
      uri,
      renderer: createExpectedFramesTrack(
        ctx,
        uri,
        track.maxDepth,
        upid,
        layerName,
      ),
      tags: {
        kinds: [SLICE_TRACK_KIND],
        upid,
      },
    });
    layerNode.addChildLast(
      new TrackNode({uri, name: EXPECTED_TIMELINE_TRACK_NAME}),
    );
  }

  private addLayerActualTracks(
    ctx: Trace,
    layerNode: TrackNode,
    upid: number,
    layerName: string,
    track: FrameTrackSummary,
  ): void {
    const uri = makeLayerUri(upid, 'actual_frames', layerName);
    ctx.tracks.registerTrack({
      uri,
      renderer: createActualFramesTrack(
        ctx,
        uri,
        track.maxDepth,
        upid,
        false,
        layerName,
      ),
      tags: {
        upid,
        kinds: [SLICE_TRACK_KIND, ACTUAL_FRAMES_SLICE_TRACK_KIND],
      },
    });
    layerNode.addChildLast(
      new TrackNode({uri, name: ACTUAL_TIMELINE_TRACK_NAME}),
    );

    // Experimental jank classification track (if enabled)
    if (!FramesPlugin.showExperimentalJankClassification.get()) return;

    const experimentalUri = makeLayerUri(
      upid,
      'actual_frames_experimental',
      layerName,
    );
    ctx.tracks.registerTrack({
      uri: experimentalUri,
      renderer: createActualFramesTrack(
        ctx,
        experimentalUri,
        track.maxDepth,
        upid,
        true,
        layerName,
      ),
      tags: {
        upid,
        kinds: [SLICE_TRACK_KIND],
      },
    });
    layerNode.addChildLast(
      new TrackNode({
        uri: experimentalUri,
        name: ACTUAL_TIMELINE_EXPERIMENTAL_TRACK_NAME,
      }),
    );
  }
}

// The frame timeline tracks of one layer of one process.
interface FrameTrackSummary {
  readonly maxDepth: number;
}

interface LayerFrameTracks {
  expected?: FrameTrackSummary;
  actual?: FrameTrackSummary;
}

// Layer name -> frame tracks, keyed by upid.
type FrameTracksByProcess = Map<number, Map<string, LayerFrameTracks>>;

// Frame slices are never nested in trace processor, so their depth is always 0.
// The rows of a layer track come from the layout the slice track computes over
// the frames of the layer: compute the same layout here to estimate its depth.
function layerMaxDepthQuery(framesByLayer: string) {
  return `
    select
      upid,
      layerName,
      max(depth) as maxDepth
    from (
      select
        upid,
        layer_name as layerName,
        internal_layout(ts, dur) over (
          partition by upid, layer_name
          order by ts
          rows between unbounded preceding and current row
        ) as depth
      from (${framesByLayer})
    )
    group by upid, layerName
  `;
}

const EXPECTED_FRAMES_BY_LAYER_QUERY = layerMaxDepthQuery(
  EXPECTED_FRAMES_BY_LAYER,
);

const ACTUAL_FRAMES_BY_LAYER_QUERY = layerMaxDepthQuery(`
  select upid, layer_name, ts, dur
  from actual_frame_timeline_slice
  where layer_name is not null and layer_name != ''
`);

async function queryFrameTracksByLayer(
  engine: Engine,
): Promise<FrameTracksByProcess> {
  const [expected, actual] = await Promise.all([
    engine.query(EXPECTED_FRAMES_BY_LAYER_QUERY),
    engine.query(ACTUAL_FRAMES_BY_LAYER_QUERY),
  ]);

  const tracksByProcess: FrameTracksByProcess = new Map();
  addLayerRows(expected, 'expected', tracksByProcess);
  addLayerRows(actual, 'actual', tracksByProcess);
  return tracksByProcess;
}

function addLayerRows(
  result: QueryResult,
  kind: keyof LayerFrameTracks,
  tracksByProcess: FrameTracksByProcess,
): void {
  const it = result.iter({
    upid: NUM,
    layerName: STR,
    maxDepth: NUM,
  });
  for (; it.valid(); it.next()) {
    const layers = getOrCreate(
      tracksByProcess,
      it.upid,
      () => new Map<string, LayerFrameTracks>(),
    );
    const layer = getOrCreate(
      layers,
      it.layerName,
      (): LayerFrameTracks => ({}),
    );
    layer[kind] = {maxDepth: it.maxDepth};
  }
}
