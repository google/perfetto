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

import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

import {ActiveCPUCountTrack, addActiveCPUCountTrack} from './active_cpu_count';
import {
  addRunnableThreadCountTrack,
  RunnableThreadCountTrack,
} from './runnable_thread_count';

class SchedPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace) {
    ctx.registerTrack({
      uri: RunnableThreadCountTrack.kind,
      trackFactory: (trackCtx) =>
        new RunnableThreadCountTrack({
          engine: ctx.engine,
          trackKey: trackCtx.trackKey,
        }),
    });
    ctx.registerTrack({
      uri: ActiveCPUCountTrack.kind,
      trackFactory: (trackCtx) => new ActiveCPUCountTrack(trackCtx, ctx.engine),
    });
  }

  onActivate(ctx: PluginContext): void {
    ctx.registerCommand({
      id: 'dev.perfetto.Sched.AddRunnableThreadCountTrackCommand',
      name: 'Add track: runnable thread count',
      callback: () => addRunnableThreadCountTrack(),
    });
    ctx.registerCommand({
      id: 'dev.perfetto.Sched.AddActiveCPUCountTrackCommand',
      name: 'Add track: active CPU count',
      callback: () => addActiveCPUCountTrack(),
    });
    for (const cpuType of ['big', 'little', 'mid']) {
      ctx.registerCommand({
        id: `dev.perfetto.Sched.AddActiveCPUCountTrackCommand.${cpuType}`,
        name: `Add track: active ${cpuType} CPU count`,
        callback: () => addActiveCPUCountTrack(cpuType),
      });
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Sched',
  plugin: SchedPlugin,
};
