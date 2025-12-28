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

import m from 'mithril';
import {LogFilteringCriteria, LogPanelCache, LogPanel} from './logs_panel';
import {ANDROID_LOGS_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {Engine} from '../../trace_processor/engine';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {createAndroidLogTrack} from './logs_track';
import {exists} from '../../base/utils';
import {TrackNode} from '../../public/workspace';
import {escapeSearchQuery} from '../../trace_processor/query_utils';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';

const VERSION = 1;

const DEFAULT_STATE: AndroidLogPluginState = {
  version: VERSION,
  filter: {
    // The first two log priorities are ignored.
    minimumLevel: 2,
    tags: [],
    isTagRegex: false,
    textEntry: '',
    hideNonMatching: true,
    machineExcludeList: [],
  },
};

interface AndroidLogPluginState {
  version: number;
  filter: LogFilteringCriteria;
}

async function getMachineIds(engine: Engine): Promise<number[]> {
  // A machine might not provide Android logs, even if configured to do so.
  // Hence, the |machine| table might have ids not present in the logs. Given this
  // is highly unlikely and going through all logs is expensive, we will get
  // the ids from |machine|, even if filter shows ids not present in logs.
  const result = await engine.query(`SELECT id FROM machine ORDER BY id`);
  const machineIds: number[] = [];
  const it = result.iter({id: NUM_NULL});
  for (; it.valid(); it.next()) {
    machineIds.push(it.id ?? 0);
  }
  return machineIds;
}

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLog';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const store = ctx.mountStore<AndroidLogPluginState>(
      'com.android.AndroidLogFilterState',
      (init) => {
        return exists(init) && (init as {version: unknown}).version === VERSION
          ? (init as AndroidLogPluginState)
          : DEFAULT_STATE;
      },
    );

    const result = await ctx.engine.query(
      `select count(1) as cnt from android_logs`,
    );
    const logCount = result.firstRow({cnt: NUM}).cnt;
    const uri = 'perfetto.AndroidLog';
    if (logCount > 0) {
      ctx.tracks.registerTrack({
        uri,
        description: () => {
          return m('', [
            'Android log (logcat) messages.',
            m('br'),
            m(
              Anchor,
              {
                href: 'https://perfetto.dev/docs/data-sources/android-log',
                target: '_blank',
                icon: Icons.ExternalLink,
              },
              'Documentation',
            ),
          ]);
        },
        tags: {kinds: [ANDROID_LOGS_TRACK_KIND]},
        renderer: createAndroidLogTrack(ctx, uri),
      });
      const track = new TrackNode({
        name: 'Android logs',
        uri,
      });
      ctx.defaultWorkspace.addChildInOrder(track);
    }

    const androidLogsTabUri = 'perfetto.AndroidLog#tab';

    // Eternal tabs should always be available even if there is nothing to show
    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as LogFilteringCriteria,
    );

    const cache: LogPanelCache = {
      uniqueMachineIds: await getMachineIds(ctx.engine),
    };

    ctx.tabs.registerTab({
      isEphemeral: false,
      uri: androidLogsTabUri,
      content: {
        render: () => m(LogPanel, {filterStore, cache, trace: ctx}),
        getTitle: () => 'Android Logs',
      },
    });

    if (logCount > 0) {
      ctx.tabs.addDefaultTab(androidLogsTabUri);
    }

    ctx.commands.registerCommand({
      id: 'com.android.ShowAndroidLogsTab',
      name: 'Show android logs tab',
      callback: () => {
        ctx.tabs.showTab(androidLogsTabUri);
      },
    });

    ctx.search.registerSearchProvider({
      name: 'Android logs',
      selectTracks(tracks) {
        return tracks
          .filter((track) =>
            track.tags?.kinds?.includes(ANDROID_LOGS_TRACK_KIND),
          )
          .filter((t) =>
            t.renderer.getDataset?.()?.implements({msg: STR_NULL}),
          );
      },
      async getSearchFilter(searchTerm) {
        return {
          where: `msg GLOB ${escapeSearchQuery(searchTerm)}`,
          columns: {msg: STR_NULL},
        };
      },
    });
  }
}
