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

import {
  COUNTER_TRACK_KIND,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {ThreadSliceTrack} from '../../frontend/thread_slice_track';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {TraceProcessorCounterTrack} from '../counter/trace_processor_counter_track';
import {THREAD_SLICE_TRACK_KIND} from '../../public';

class AnnotationPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addAnnotationTracks(ctx);
    await this.addAnnotationCounterTracks(ctx);
  }

  private async addAnnotationTracks(ctx: PluginContextTrace) {
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

    for (; it.valid(); it.next()) {
      const {id, name, upid, groupName} = it;

      ctx.registerTrack({
        uri: `/annotation_${id}`,
        title: name,
        tags: {
          kind: THREAD_SLICE_TRACK_KIND,
          scope: 'annotation',
          upid,
          ...(groupName && {groupName}),
        },
        chips: ['metric'],
        trackFactory: ({trackKey}) => {
          return new ThreadSliceTrack(
            {
              engine: ctx.engine,
              trackKey,
            },
            id,
            0,
            'annotation_slice',
          );
        },
      });
    }
  }

  private async addAnnotationCounterTracks(ctx: PluginContextTrace) {
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

      ctx.registerTrack({
        uri: `/annotation_counter_${trackId}`,
        title: name,
        tags: {
          kind: COUNTER_TRACK_KIND,
          scope: 'annotation',
          upid,
        },
        chips: ['metric'],
        trackFactory: (trackCtx) => {
          return new TraceProcessorCounterTrack({
            engine: ctx.engine,
            trackKey: trackCtx.trackKey,
            trackId,
            rootTable: 'annotation_counter',
          });
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Annotation',
  plugin: AnnotationPlugin,
};
