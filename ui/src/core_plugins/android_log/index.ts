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
import {Plugin, PluginContextTrace, PluginDescriptor} from '../../public';
import {NUM} from '../../trace_processor/query_result';
import {AndroidLogTrack} from './logs_track';

export const ANDROID_LOGS_TRACK_KIND = 'AndroidLogTrack';

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

class AndroidLog implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const store = ctx.mountStore<AndroidLogPluginState>((init) => {
      return init && (init as {version: unknown}).version === VERSION
        ? (init as AndroidLogPluginState)
        : DEFAULT_STATE;
    });

    const result = await ctx.engine.query(
      `select count(1) as cnt from android_logs`,
    );
    const logCount = result.firstRow({cnt: NUM}).cnt;
    if (logCount > 0) {
      ctx.registerStaticTrack({
        uri: 'perfetto.AndroidLog',
        displayName: 'Android logs',
        kind: ANDROID_LOGS_TRACK_KIND,
        trackFactory: () => new AndroidLogTrack(ctx.engine),
      });
    }

    const androidLogsTabUri = 'perfetto.AndroidLog#tab';

    // Eternal tabs should always be available even if there is nothing to show
    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as LogFilteringCriteria,
    );

    ctx.registerTab({
      isEphemeral: false,
      uri: androidLogsTabUri,
      content: {
        render: () =>
          m(LogPanel, {filterStore: filterStore, engine: ctx.engine}),
        getTitle: () => 'Android Logs',
      },
    });

    if (logCount > 0) {
      ctx.addDefaultTab(androidLogsTabUri);
    }

    ctx.registerCommand({
      id: 'perfetto.AndroidLog#ShowLogsTab',
      name: 'Show android logs tab',
      callback: () => {
        ctx.tabs.showTab(androidLogsTabUri);
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.AndroidLog',
  plugin: AndroidLog,
};
