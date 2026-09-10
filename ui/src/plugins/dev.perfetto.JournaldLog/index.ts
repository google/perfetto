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

import m from 'mithril';
import {JOURNALD_FILTER_SCHEMA, JournaldLogPanel} from './logs_panel';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {createJournaldLogTrack} from './logs_track';
import {TrackNode} from '../../public/workspace';
import {escapeSearchQuery} from '../../trace_processor/query_utils';
import {z} from 'zod';

const JOURNALD_LOGS_TRACK_KIND = 'JournaldLogTrack';

const JOURNALD_STATE_SCHEMA = z
  .object({
    version: z.number().default(1),
    filter: JOURNALD_FILTER_SCHEMA,
  })
  .prefault({});

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.JournaldLog';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const filterStore = ctx.registerStorage({
      id: 'dev.perfetto.JournaldLogFilterState',
      schema: JOURNALD_STATE_SCHEMA,
    });

    const result = await ctx.engine.query(`
      INCLUDE PERFETTO MODULE linux.systemd_journald;
      select count(1) as cnt from linux_systemd_journald_logs;
    `);
    const logCount = result.firstRow({cnt: NUM}).cnt;
    const uri = 'perfetto.JournaldLog';
    if (logCount > 0) {
      ctx.tracks.registerTrack({
        uri,
        tags: {kinds: [JOURNALD_LOGS_TRACK_KIND]},
        renderer: createJournaldLogTrack(ctx, uri),
      });
      const track = new TrackNode({
        name: 'Journald logs',
        uri,
      });
      ctx.defaultWorkspace.addChildInOrder(track);
    }

    const journaldLogsTabUri = 'perfetto.JournaldLog#tab';

    ctx.tabs.registerTab({
      isEphemeral: false,
      uri: journaldLogsTabUri,
      content: {
        render: () =>
          m(JournaldLogPanel, {
            trace: ctx,
            filter: filterStore.get().filter,
            onFilterChange: (filter) =>
              filterStore.set({...filterStore.get(), filter}),
          }),
        getTitle: () => 'Journald Logs',
      },
    });

    if (logCount > 0) {
      ctx.tabs.addDefaultTab(journaldLogsTabUri);
    }

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ShowJournaldLogsTab',
      name: 'Show journald logs tab',
      callback: () => {
        ctx.tabs.showTab(journaldLogsTabUri);
      },
    });

    ctx.search.registerSearchProvider({
      name: 'Journald logs',
      selectTracks(tracks) {
        return tracks
          .filter((track) =>
            track.tags?.kinds?.includes(JOURNALD_LOGS_TRACK_KIND),
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
