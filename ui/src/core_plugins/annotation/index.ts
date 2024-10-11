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

import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';
import {ThreadSliceTrack} from '../../frontend/thread_slice_track';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {TraceProcessorCounterTrack} from '../counter/trace_processor_counter_track';
import {THREAD_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {TrackNode, type TrackNodeContainer} from '../../public/workspace';
import {getOrCreateGroupForProcess} from '../../public/standard_groups';

class AnnotationPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.addAnnotationTracks(ctx);
    await this.addAnnotationCounterTracks(ctx);
  }

  private async addAnnotationTracks(ctx: Trace) {
    const {engine} = ctx;

    const result = await engine.query(`
      select
        id,
        name,
        upid,
        group_name as groupName
      from annotation_slice_track
      order by name
    `);

    const it = result.iter({
      id: NUM,
      name: STR,
      upid: NUM,
      groupName: STR_NULL,
    });

    const groups = new Map<string, TrackNode>();

    for (; it.valid(); it.next()) {
      const {id, name, upid, groupName} = it;

      const uri = `/annotation_${id}`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: THREAD_SLICE_TRACK_KIND,
          scope: 'annotation',
          upid,
        },
        chips: ['metric'],
        track: new ThreadSliceTrack(
          {
            trace: ctx,
            uri,
          },
          id,
          0,
          'annotation_slice',
        ),
      });

      // We want to try and find a group to put this track in. If groupName is
      // defined, create a new group or place in existing one if it already
      // exists Otherwise, try upid to see if we can put this in a process
      // group

      let container: TrackNodeContainer;
      if (groupName) {
        const existingGroup = groups.get(groupName);
        if (!existingGroup) {
          const group = new TrackNode({title: groupName, uri, isSummary: true});
          container = group;
          groups.set(groupName, group);
          ctx.workspace.addChildInOrder(group);
        } else {
          container = existingGroup;
        }
      } else {
        if (upid !== 0) {
          container = getOrCreateGroupForProcess(ctx.workspace, upid);
        } else {
          container = ctx.workspace;
        }
      }

      const track = new TrackNode({uri, title: name});
      container.addChildInOrder(track);
    }
  }

  private async addAnnotationCounterTracks(ctx: Trace) {
    const {engine} = ctx;
    const counterResult = await engine.query(`
      SELECT
        id,
        name,
        min_value as minValue,
        max_value as maxValue,
        upid
      FROM annotation_counter_track`);

    const counterIt = counterResult.iter({
      id: NUM,
      name: STR,
      minValue: NUM_NULL,
      maxValue: NUM_NULL,
      upid: NUM,
    });

    for (; counterIt.valid(); counterIt.next()) {
      const {id: trackId, name, upid} = counterIt;

      const uri = `/annotation_counter_${trackId}`;
      ctx.tracks.registerTrack({
        uri,
        title: name,
        tags: {
          kind: COUNTER_TRACK_KIND,
          scope: 'annotation',
          upid,
        },
        chips: ['metric'],
        track: new TraceProcessorCounterTrack({
          trace: ctx,
          uri,
          trackId,
          rootTable: 'annotation_counter',
        }),
      });

      const group = getOrCreateGroupForProcess(ctx.workspace, upid);
      const track = new TrackNode({uri, title: name});
      group.addChildInOrder(track);
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Annotation',
  plugin: AnnotationPlugin,
};
