// Copyright (C) 2025 The Android Open Source Project
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
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';

export default class implements PerfettoPlugin {
  static readonly id = 'com.google.YouTubeTrace';

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.commands.registerCommand({
      id: `com.google.YouTubePinJankTracksMainThread`,
      name: 'YT CUJ: Pin Jank Tracks (main thread)',
      callback: async () => {
        if (await this.tryIncludeCommonModule(trace)) {
          this.pinDebugTrack(
            trace,
            'SELECT * FROM reconstruct_imp_duration_spans(1)',
            'YT Jank: Imp Duration (main thread)',
          );
          this.pinDebugTrack(
            trace,
            'SELECT * FROM recreate_scroll_duration_spans(1)',
            'YT Jank: Scroll Duration (main thread)',
          );
          this.pinDebugTrack(
            trace,
            'SELECT * FROM merged_local_directors(1)',
            'YT Jank: Merged Local Director (main thread)',
          );
        }
      },
    });

    trace.commands.registerCommand({
      id: `com.google.YouTubePinJankTracksAllThreads`,
      name: 'YT CUJ: Pin Jank Tracks (all threads)',
      callback: async () => {
        if (await this.tryIncludeCommonModule(trace)) {
          this.pinDebugTrack(
            trace,
            'SELECT * FROM reconstruct_imp_duration_spans(0)',
            'YT Jank: Imp Duration (all threads)',
          );
          this.pinDebugTrack(
            trace,
            'SELECT * FROM recreate_scroll_duration_spans(0)',
            'YT Jank: Scroll Duration (all threads)',
          );
          this.pinDebugTrack(
            trace,
            'SELECT * FROM merged_local_directors(0)',
            'YT Jank: Merged Local Director (all threads)',
          );
        }
      },
    });
  }
  private pinDebugTrack(trace: Trace, query: string, trackName: string) {
    if (
      !trace.currentWorkspace.pinnedTracks.find((t) => t.name === trackName)
    ) {
      trace.commands.runCommand(
        'dev.perfetto.AddDebugSliceTrack',
        query,
        trackName,
      );
    }
  }

  private async tryIncludeCommonModule(trace: Trace) {
    try {
      await trace.engine.query(
        `INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.imp`,
      );
      await trace.engine.query(
        `INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.scroll`,
      );
      await trace.engine.query(
        `INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.local_director`,
      );
      return true;
    } catch (e) {
      return false;
    }
  }
}
