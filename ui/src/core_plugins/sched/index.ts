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

import {uuidv4} from '../../base/uuid';
import {Actions} from '../../common/actions';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {globals} from '../../frontend/globals';
import {
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
} from '../../public';

import {ActiveCPUCountTrack, CPUType} from './active_cpu_count';
import {RunnableThreadCountTrack} from './runnable_thread_count';

class SchedPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace) {
    const runnableThreadCountUri = `/runnable_thread_count`;
    ctx.registerTrack({
      uri: runnableThreadCountUri,
      title: 'Runnable thread count',
      trackFactory: (trackCtx) =>
        new RunnableThreadCountTrack({
          engine: ctx.engine,
          trackKey: trackCtx.trackKey,
        }),
    });
    ctx.registerCommand({
      id: 'dev.perfetto.Sched.AddRunnableThreadCountTrackCommand',
      name: 'Add track: runnable thread count',
      callback: () =>
        addPinnedTrack(runnableThreadCountUri, 'Runnable thread count'),
    });

    const uri = uriForActiveCPUCountTrack();
    const title = 'Active CPU count';
    ctx.registerTrack({
      uri,
      title: title,
      trackFactory: (trackCtx) => new ActiveCPUCountTrack(trackCtx, ctx.engine),
    });
    ctx.registerCommand({
      id: 'dev.perfetto.Sched.AddActiveCPUCountTrackCommand',
      name: 'Add track: active CPU count',
      callback: () => addPinnedTrack(uri, title),
    });

    for (const cpuType of Object.values(CPUType)) {
      const uri = uriForActiveCPUCountTrack(cpuType);
      const title = `Active ${cpuType} CPU count`;
      ctx.registerTrack({
        uri,
        title: title,
        trackFactory: (trackCtx) =>
          new ActiveCPUCountTrack(trackCtx, ctx.engine, cpuType),
      });

      ctx.registerCommand({
        id: `dev.perfetto.Sched.AddActiveCPUCountTrackCommand.${cpuType}`,
        name: `Add track: active ${cpuType} CPU count`,
        callback: () => addPinnedTrack(uri, title),
      });
    }
  }
}

function uriForActiveCPUCountTrack(cpuType?: CPUType): string {
  const prefix = `/active_cpus`;
  if (cpuType !== undefined) {
    return `${prefix}_${cpuType}`;
  } else {
    return prefix;
  }
}

function addPinnedTrack(uri: string, title: string) {
  const key = uuidv4();
  globals.dispatchMultiple([
    Actions.addTrack({
      key,
      uri,
      name: title,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
    }),
    Actions.toggleTrackPinned({trackKey: key}),
  ]);
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Sched',
  plugin: SchedPlugin,
};
