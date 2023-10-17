// Copyright (C) 2022 The Android Open Source Project
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

// import {NewTrackArgs, Track} from '../../frontend/track';
// import {TrackButton, TrackButtonAttrs} from '../../frontend/track_panel';
import m from 'mithril';
import {v4 as uuidv4} from 'uuid';

import {Actions} from '../../common/actions';
import {globals} from '../../frontend/globals';
import {TrackButton} from '../../frontend/track_panel';
import {
  EngineProxy,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {ChromeSliceTrack} from '../chrome_slices';

export const VISUALISED_ARGS_SLICE_TRACK_KIND = 'VisualisedArgsTrack';
export const VISUALISED_ARGS_SLICE_TRACK_URI = 'perfetto.VisualisedArgs';

export interface VisualisedArgsState {
  argName: string;
  maxDepth: number;
  trackId: number;
}

export class VisualisedArgsTrack extends ChromeSliceTrack {
  private helperViewName: string;

  constructor(
      engine: EngineProxy, maxDepth: number, trackInstanceId: string,
      trackId: number, private argName: string) {
    const uuid = uuidv4();
    const namespace = `__arg_visualisation_helper_${argName}_${uuid}`;
    const escapedNamespace = namespace.replace(/[^a-zA-Z]/g, '_');
    super(engine, maxDepth, trackInstanceId, trackId, escapedNamespace);
    this.helperViewName = `${escapedNamespace}_slice`;
  }

  async onCreate(): Promise<void> {
    // Create the helper view - just one which is relevant to this slice
    await this.engine.query(`
          create view ${this.helperViewName} as
          with slice_with_arg as (
            select
              slice.id,
              slice.track_id,
              slice.ts,
              slice.dur,
              slice.thread_dur,
              NULL as cat,
              args.display_value as name
            from slice
            join args using (arg_set_id)
            where args.key='${this.argName}'
          )
          select
            *,
            (select count()
            from ancestor_slice(s1.id) s2
            join slice_with_arg s3 on s2.id=s3.id
            ) as depth
          from slice_with_arg s1
          order by id;
      `);
  }

  async onDestroy(): Promise<void> {
    this.engine.query(`drop view ${this.helperViewName}`);
  }

  getFont() {
    return 'italic 11px Roboto';
  }

  getTrackShellButtons(): m.Children {
    return m(TrackButton, {
      action: () => {
        // This behavior differs to the original behavior a little.
        // Originally, hitting the close button on a single track removed ALL
        // tracks with this argName, whereas this one only closes the single
        // track.
        // This will be easily fixable once we transition to using dynamic
        // tracks instead of this "initial state" approach to add these tracks.
        globals.dispatch(
            Actions.removeTracks({trackInstanceIds: [this.trackInstanceId]}));
      },
      i: 'close',
      tooltip: 'Close',
      showButton: true,
    });
  }
}

class VisualisedArgsPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}
  async onTraceLoad(ctx: PluginContextTrace<undefined>): Promise<void> {
    ctx.addTrack({
      uri: VISUALISED_ARGS_SLICE_TRACK_URI,
      displayName: 'Visualised Args',
      kind: VISUALISED_ARGS_SLICE_TRACK_KIND,
      tags: {
        metric: true,  // TODO(stevegolton): Is this track really a metric?
      },
      track: (trackCtx) => {
        // Mount the store and migrate initial state.
        const store = trackCtx.mountStore((initialState) => {
          // TODO(stevegolton): Check initialState properly. Note, this is no
          // worse than the situation we had before with track config.
          // When we migrate to "proper" dynamic tracks, the problem of
          // migrating state will be pushed up to the plugin anyway.
          return initialState as VisualisedArgsState;
        });
        return new VisualisedArgsTrack(
            ctx.engine,
            store.state.maxDepth,
            trackCtx.trackInstanceId,
            store.state.trackId,
            store.state.argName,
        );
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.VisualisedArgs',
  plugin: VisualisedArgsPlugin,
};
