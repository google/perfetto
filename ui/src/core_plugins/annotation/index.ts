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

import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {ThreadSliceTrack} from '../../frontend/thread_slice_track';
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {COUNTER_TRACK_KIND} from '../counter';
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
      select id, name
      from annotation_slice_track
      order by name
    `);

    const it = result.iter({
      id: NUM,
      name: STR,
    });

    for (; it.valid(); it.next()) {
      const id = it.id;
      const name = it.name;

      ctx.registerTrack({
        uri: `perfetto.Annotation#${id}`,
        displayName: name,
        kind: THREAD_SLICE_TRACK_KIND,
        tags: {
          metric: true,
        },
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
        max_value as maxValue
      FROM annotation_counter_track`);

    const counterIt = counterResult.iter({
      id: NUM,
      name: STR,
      minValue: NUM_NULL,
      maxValue: NUM_NULL,
    });

    for (; counterIt.valid(); counterIt.next()) {
      const trackId = counterIt.id;
      const name = counterIt.name;

      ctx.registerTrack({
        uri: `perfetto.Annotation#counter${trackId}`,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        tags: {
          metric: true,
        },
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
