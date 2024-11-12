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
import {LogFilteringCriteria, LogPanel} from './logs_panel';
import {ANDROID_LOGS_TRACK_KIND} from '../../public/track_kinds';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {sqlTableRegistry} from '../../frontend/widgets/sql/table/sql_table_registry';
import {NUM} from '../../trace_processor/query_result';
import {AndroidLogTrack} from './logs_track';
import {exists} from '../../base/utils';
import {TrackNode} from '../../public/workspace';
import {getAndroidLogsTable} from './table';
import {extensions} from '../../public/lib/extensions';

const VERSION = 1;

const DEFAULT_STATE: AndroidLogPluginState = {
  version: VERSION,
  filter: {
    // The first two log priorities are ignored.
    minimumLevel: 2,
    tags: [],
    textEntry: '',
    hideNonMatching: true,
  },
};

interface AndroidLogPluginState {
  version: number;
  filter: LogFilteringCriteria;
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AndroidLog';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const store = ctx.mountStore<AndroidLogPluginState>((init) => {
      return exists(init) && (init as {version: unknown}).version === VERSION
        ? (init as AndroidLogPluginState)
        : DEFAULT_STATE;
    });

    const result = await ctx.engine.query(
      `select count(1) as cnt from android_logs`,
    );
    const logCount = result.firstRow({cnt: NUM}).cnt;
    const uri = 'perfetto.AndroidLog';
    const title = 'Android logs';
    if (logCount > 0) {
      ctx.tracks.registerTrack({
        uri,
        title,
        tags: {kind: ANDROID_LOGS_TRACK_KIND},
        track: new AndroidLogTrack(ctx.engine),
      });
      const track = new TrackNode({title, uri});
      ctx.workspace.addChildInOrder(track);
    }

    const androidLogsTabUri = 'perfetto.AndroidLog#tab';

    // Eternal tabs should always be available even if there is nothing to show
    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as LogFilteringCriteria,
    );

    ctx.tabs.registerTab({
      isEphemeral: false,
      uri: androidLogsTabUri,
      content: {
        render: () => m(LogPanel, {filterStore: filterStore, trace: ctx}),
        getTitle: () => 'Android Logs',
      },
    });

    if (logCount > 0) {
      ctx.tabs.addDefaultTab(androidLogsTabUri);
    }

    ctx.commands.registerCommand({
      id: 'perfetto.AndroidLog#ShowLogsTab',
      name: 'Show android logs tab',
      callback: () => {
        ctx.tabs.showTab(androidLogsTabUri);
      },
    });

    sqlTableRegistry['android_logs'] = getAndroidLogsTable();
    ctx.commands.registerCommand({
      id: 'perfetto.ShowTable.android_logs',
      name: 'Open table: android_logs',
      callback: () => {
        extensions.addSqlTableTab(ctx, {
          table: getAndroidLogsTable(),
        });
      },
    });
  }
}
