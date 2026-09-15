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
import {createExpectedFramesTrack} from './expected_frames_track';
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

export default class FramesPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Frames';
  static readonly dependencies = [ProcessThreadGroupsPlugin];
  static showExperimentalJankClassification: Setting<boolean>;
  static groupFramesByLayer: Setting<boolean>;

  static onActivate(app: App): void {
    FramesPlugin.showExperimentalJankClassification = app.settings.register({
      id: `${FramesPlugin.id}#showExperimentalJankClassification`,
      name: 'show experimental jank classification track (alpha)',
      description: 'Use alternative method to classify jank. Not recommented.',
      schema: z.boolean(),
      defaultValue: false,
      requiresReload: true,
    });
    FramesPlugin.groupFramesByLayer = app.settings.register({
      id: `${FramesPlugin.id}#groupFramesByLayer`,
      name: 'Group frame timelines by layer',
      description:
        'Generate Expected Timeline and Actual Timeline tracks grouped by layer name.',
      schema: z.boolean(),
      defaultValue: false,
      requiresReload: true,
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    await Promise.all([this.addExpectedFrames(ctx), this.addActualFrames(ctx)]);
    if (FramesPlugin.groupFramesByLayer.get()) {
      await this.addFramesByLayer(ctx);
    }
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new FrameSelectionAggregator(ctx), 10),
    );
  }

  async addExpectedFrames(ctx: Trace): Promise<void> {
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
        renderer: createExpectedFramesTrack(ctx, uri, maxDepth, trackIds),
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

  async addActualFrames(ctx: Trace): Promise<void> {
    const {engine} = ctx;
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
          trackIds,
          false,
        ),
        tags: {
          upid,
          trackIds,
          kinds: [SLICE_TRACK_KIND, ACTUAL_FRAMES_SLICE_TRACK_KIND],
        },
      });
      group?.addChildInOrder(
        new TrackNode({
          uri: standardUri,
          name: ACTUAL_TIMELINE_TRACK_NAME,
          sortOrder: TIMELINE_SORT_ORDER,
        }),
      );

      // Experimental jank classification track (if enabled)
      if (FramesPlugin.showExperimentalJankClassification.get()) {
        const experimentalUri = makeUri(upid, 'actual_frames_experimental');
        ctx.tracks.registerTrack({
          uri: experimentalUri,
          renderer: createActualFramesTrack(
            ctx,
            experimentalUri,
            maxDepth,
            trackIds,
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
  async addFramesByLayer(ctx: Trace): Promise<void> {
    const tracksByProcess = await queryFrameTracksByLayer(ctx.engine);
    const processThreadGroups = ctx.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    );

    for (const [upid, layers] of tracksByProcess) {
      const processGroup = processThreadGroups.getGroupForProcess(upid);
      if (processGroup === undefined) continue;

      const actualTrack = processGroup.getTrackByUri(
        makeUri(upid, 'actual_frames'),
      );
      const parentNode = actualTrack ?? processGroup;

      const sortedLayers = Array.from(layers).sort(([a], [b]) =>
        a.localeCompare(b),
      );
      for (const [layerName, tracks] of sortedLayers) {
        parentNode.addChildLast(
          this.createLayerNode(ctx, upid, layerName, tracks),
        );
      }
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
        track.trackIds,
        {name: layerName, upid},
      ),
      tags: {
        kinds: [SLICE_TRACK_KIND],
        trackIds: track.trackIds,
        upid,
        layerName,
      },
    });
    layerNode.addChildInOrder(
      new TrackNode({
        uri,
        name: EXPECTED_TIMELINE_TRACK_NAME,
        sortOrder: TIMELINE_SORT_ORDER,
      }),
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
        track.trackIds,
        false,
        {name: layerName, upid},
      ),
      tags: {
        upid,
        trackIds: track.trackIds,
        layerName,
        kinds: [SLICE_TRACK_KIND, ACTUAL_FRAMES_SLICE_TRACK_KIND],
      },
    });
    layerNode.addChildInOrder(
      new TrackNode({
        uri,
        name: ACTUAL_TIMELINE_TRACK_NAME,
        sortOrder: TIMELINE_SORT_ORDER,
      }),
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
        track.trackIds,
        true,
        {name: layerName, upid},
      ),
      tags: {
        upid,
        trackIds: track.trackIds,
        layerName,
        kinds: [SLICE_TRACK_KIND],
      },
    });
    layerNode.addChildInOrder(
      new TrackNode({
        uri: experimentalUri,
        name: ACTUAL_TIMELINE_EXPERIMENTAL_TRACK_NAME,
        sortOrder: EXPERIMENTAL_TIMELINE_SORT_ORDER,
      }),
    );
  }
}

// The frame timeline tracks of one layer of one process.
interface FrameTrackSummary {
  readonly trackIds: number[];
  readonly maxDepth: number;
}

interface LayerFrameTracks {
  expected?: FrameTrackSummary;
  actual?: FrameTrackSummary;
}

// Layer name -> frame tracks, keyed by upid.
type FrameTracksByProcess = Map<number, Map<string, LayerFrameTracks>>;

// Expected frames are usually tagged with the name of the layer they belong to,
// but some traces only emit them for one of the layers of a process. Frames
// tagged with another layer name are attributed to this layer via the surface
// frame token of the corresponding actual frame, which is unique in a process.
const EXPECTED_FRAMES_BY_LAYER_QUERY = `
  with layer_expected as (
    select upid, layer_name, track_id, depth
    from expected_frame_timeline_slice
    where layer_name is not null

    union

    select act.upid, act.layer_name, exp.track_id, exp.depth
    from actual_frame_timeline_slice act
    join expected_frame_timeline_slice exp
      on act.upid = exp.upid
      and act.surface_frame_token = exp.surface_frame_token
    where act.layer_name is not null
      and act.surface_frame_token is not null
  )
  select
    upid,
    layer_name as layerName,
    group_concat(distinct track_id) as trackIds,
    ifnull(max(depth), 0) as maxDepth
  from layer_expected
  group by upid, layer_name
`;

const ACTUAL_FRAMES_BY_LAYER_QUERY = `
  select
    upid,
    layer_name as layerName,
    group_concat(distinct track_id) as trackIds,
    ifnull(max(depth), 0) as maxDepth
  from actual_frame_timeline_slice
  where layer_name is not null
  group by upid, layer_name
`;

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
    trackIds: STR,
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
    layer[kind] = {
      trackIds: it.trackIds.split(',').filter(Boolean).map(Number),
      maxDepth: it.maxDepth,
    };
  }
}
