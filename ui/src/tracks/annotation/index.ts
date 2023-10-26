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
  NUM,
  NUM_NULL,
  STR,
} from '../../common/query_result';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {ChromeSliceTrack, SLICE_TRACK_KIND} from '../chrome_slices/';
import {
  Config as CounterTrackConfig,
  COUNTER_TRACK_KIND,
  CounterTrack,
} from '../counter';

class AnnotationPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

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

      ctx.registerStaticTrack({
        uri: `perfetto.Annotation#${id}`,
        displayName: name,
        kind: SLICE_TRACK_KIND,
        tags: {
          metric: true,
        },
        track: (({trackKey}) => {
          return new ChromeSliceTrack(
              engine,
              0,
              trackKey,
              id,
              'annotation',
          );
        }),
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
      const id = counterIt.id;
      const name = counterIt.name;
      const minimumValue =
          counterIt.minValue === null ? undefined : counterIt.minValue;
      const maximumValue =
          counterIt.maxValue === null ? undefined : counterIt.maxValue;

      const config: CounterTrackConfig = {
        name,
        trackId: id,
        namespace: 'annotation',
        minimumValue,
        maximumValue,
      };

      ctx.registerStaticTrack({
        uri: `perfetto.Annotation#counter${id}`,
        displayName: name,
        kind: COUNTER_TRACK_KIND,
        tags: {
          metric: true,
        },
        track: (trackCtx) => {
          return new CounterTrack(trackCtx, config, ctx.engine);
        },
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Annotation',
  plugin: AnnotationPlugin,
};
