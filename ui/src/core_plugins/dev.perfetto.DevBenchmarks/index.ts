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

import {raf} from '../../core/raf_scheduler';
import {TraceImpl} from '../../core/trace_impl';
import {PerfettoPlugin} from '../../public/plugin';

export default class DevBenchmarks implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.DevBenchmarks';

  async onTraceLoad(ctx: TraceImpl): Promise<void> {
    ctx.commands.registerCommand({
      id: 'dev.perfetto.DevBenchmarks.RunBenchmark',
      name: 'Run Dev Benchmark',
      callback: () => {
        // 1. Hide the sidebar
        if (ctx.sidebar.visible) {
          ctx.sidebar.toggleVisibility();
        }

        // 2. Expand ftrace events
        // 3. Collapse cpu frequency
        ctx.currentWorkspace.flatTracks.forEach((track) => {
          const name = track.name.toLowerCase();
          if (name.includes('ftrace events')) {
            track.expand();
          }
          if (name.includes('cpu frequency')) {
            track.collapse();
          }
        });

        // 4. Hide the drawer
        ctx.tabs.toggleTabPanelVisibility();

        // 5. Enable perf metrics
        ctx.perfDebugging.enabled = true;

        // 6. Render some frames
        let redraws = 0;
        const targetRedraws = 500;

        const callback = () => {
          if (redraws++ >= targetRedraws) {
            raf.stopAnimation(callback);
          }
        };

        raf.startAnimation(callback);
      },
    });
  }
}
