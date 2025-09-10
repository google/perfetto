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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.google.YouTubeTrace';

  async onTraceLoad(trace: Trace): Promise<void> {
    // Fetch the config from the trace, or fallback to the default
    // config if it's not available.
    const config = (await this.fetchConfig(trace)) || this.defaultConfig;
    if (!config) {
      return;
    }

    if (await this.tryIncludeModules(trace, config.includeModules)) {
      for (const command of config.commands) {
        trace.commands.registerCommand({
          id: command.id,
          name: command.name,
          callback: () => {
            for (const track of command.tracks) {
              this.pinDebugTrack(trace, track.query, track.trackName);
            }
          },
        });
      }
    }
  }

  private pinDebugTrack(trace: Trace, query: string, trackName: string) {
    if (!trace.workspace.pinnedTracks.find((t) => t.name === trackName)) {
      trace.commands.runCommand(
        'dev.perfetto.AddDebugSliceTrack',
        query,
        trackName,
      );
    }
  }

  private async tryIncludeModules(trace: Trace, queries: string[]) {
    try {
      for (const query of queries) {
        await trace.engine.query(query);
      }
      return true;
    } catch (e) {
      return false;
    }
  }

  private async fetchConfig(
    trace: Trace,
  ): Promise<PerfettoPluginConfig | null> {
    if (
      !(await this.tryIncludeModules(trace, [
        'INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_config.cuj_jank_tracks_config',
      ]))
    ) {
      return null;
    }

    try {
      const queryResult = await trace.engine.query(
        'SELECT yt_plugin_config_json() AS config',
      );
      const it = queryResult.iter({
        config: STR,
      });
      if (it.valid()) {
        return this.parseConfig(it.config);
      }
    } catch (e) {
      return null;
    }
    return null;
  }

  private parseConfig(configString: string): PerfettoPluginConfig | null {
    try {
      return JSON.parse(configString) as PerfettoPluginConfig;
    } catch (e) {
      return null;
    }
  }

  readonly defaultConfig: PerfettoPluginConfig | null = this.parseConfig(`
      {
        "includeModules": [
          "INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.scroll",
          "INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.imp",
          "INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.local_director",
          "INCLUDE PERFETTO MODULE google3.video.youtube.analytics.client_apps.system_health.tools.trace.perfetto_module.eml_xml"
        ],
        "commands": [
          {
            "id": "com.google.YouTubePinJankTracksMainThread",
            "name": "YT CUJ: Pin Jank Tracks (main thread)",
            "tracks": [
              {
                "query": "SELECT * FROM recreate_scroll_duration_spans(1)",
                "trackName": "YT Jank: Scroll Duration (main thread)"
              },
              {
                "query": "SELECT * FROM merged_eml_xml_spans(1)",
                "trackName": "YT Jank: Merged EML/XML (main thread)"
              },
              {
                "query": "SELECT * FROM reconstruct_imp_duration_spans(1)",
                "trackName": "YT Jank: Imp Duration (main thread)"
              },
              {
                "query": "SELECT * FROM merged_local_directors(1)",
                "trackName": "YT Jank: Merged Local Director (main thread)"
              }
            ]
          },
          {
            "id": "com.google.YouTubePinJankTracksAllThreads",
            "name": "YT CUJ: Pin Jank Tracks (all threads)",
            "tracks": [
              {
                "query": "SELECT * FROM recreate_scroll_duration_spans(0)",
                "trackName": "YT Jank: Scroll Duration (all threads)"
              },
              {
                "query": "SELECT * FROM reconstruct_imp_duration_spans(0)",
                "trackName": "YT Jank: Imp Duration (all threads)"
              },
              {
                "query": "SELECT * FROM merged_local_directors(0)",
                "trackName": "YT Jank: Merged Local Director (all threads)"
              },
              {
                "query": "SELECT * FROM merged_eml_xml_spans(0)",
                "trackName": "YT Jank: Merged EML/XML (all threads)"
              }
            ]
          }
        ]
      }`);
}

declare interface TrackConfig {
  query: string;
  trackName: string;
}

declare interface CommandConfig {
  id: string;
  name: string;
  tracks: TrackConfig[];
}

declare interface PerfettoPluginConfig {
  includeModules: string[];
  commands: CommandConfig[];
}
