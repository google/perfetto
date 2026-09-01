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

import './styles.scss';
import m from 'mithril';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  type LogFilteringCriteria,
  type LogPanelCache,
  LogPanel,
} from './logs_panel';
import {ANDROID_LOGS_TRACK_KIND} from '../../public/track_kinds';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Engine} from '../../trace_processor/engine';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {createPerProcessLogTrack, createPerThreadLogTrack} from './logs_track';
import {exists} from '../../base/utils';
import {TrackNode} from '../../public/workspace';
import {escapeSearchQuery} from '../../trace_processor/query_utils';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {AndroidLogSelectionAggregator} from './log_selection_aggregator';
import {getMachineCount, maybeMachineLabel} from '../../public/utils';

const VERSION = 1;

const DEFAULT_STATE: JournalLogPluginState = {
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

interface JournalLogPluginState {
  version: number;
  filter: LogFilteringCriteria;
}

async function getMachines(engine: Engine): Promise<LogPanelCache> {
  // A machine might not provide Android logs, even if configured to do so.
  // Hence, the |machine| table might have ids not present in the logs. Given this
  // is highly unlikely and going through all logs is expensive, we will get
  // the ids from |machine|, even if filter shows ids not present in logs.
  const result = await engine.query(`
    SELECT id, name
    FROM machine
    ORDER BY id
  `);
  const machineIds: number[] = [];
  const machineNames = new Map<number, string>();
  const it = result.iter({id: NUM_NULL, name: STR_NULL});
  for (; it.valid(); it.next()) {
    const id = it.id ?? 0;
    machineIds.push(id);
    machineNames.set(id, it.name || `Machine ${id}`);
  }
  return {uniqueMachineIds: machineIds, machineNames};
}

const THREADS_QUERY = `
  SELECT
    al.utid,
    t.upid,
    t.tid,
    p.pid,
    p.name AS process_name,
    t.name AS thread_name,
    t.machine_id AS machine_id,
    machine.name AS machine_name,
    machine.label_index AS machine_label_index,
    count() AS log_count
  FROM android_logs al
  LEFT JOIN thread t ON al.utid = t.utid
  LEFT JOIN process p ON t.upid = p.upid
  LEFT JOIN machine ON machine.id = t.machine_id
  GROUP BY al.utid
  ORDER BY log_count DESC
`;

interface ProcessGroup {
  processName: string;
  pid: number | null;
  logCount: number;
  machineId: number;
  machineName: string | null;
  machineLabelIndex: number | null;
  threads: Array<{
    utid: number;
    upid: number | null;
    threadName: string;
    logCount: number;
  }>;
}

export default class implements PerfettoPlugin {
  static readonly id = 'org.rdk.JournalLog';
  async onTraceLoad(ctx: Trace): Promise<void> {
    const numMachines = await getMachineCount(ctx.engine);
    const store = ctx.mountStore<JournalLogPluginState>(
      'org.rdk.JournalLogFilterState',
      (init) => {
        return exists(init) && (init as {version: unknown}).version === VERSION
          ? (init as JournalLogPluginState)
          : DEFAULT_STATE;
      },
    );

    const result = await ctx.engine.query(
      `select count(1) as cnt from android_logs`,
    );
    const logCount = result.firstRow({cnt: NUM}).cnt;

    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new AndroidLogSelectionAggregator(ctx)),
    );

    if (logCount > 0) {
      const threads = await ctx.engine.query(THREADS_QUERY);
      const it = threads.iter({
        utid: NUM,
        upid: NUM_NULL,
        tid: NUM_NULL,
        pid: NUM_NULL,
        process_name: STR_NULL,
        thread_name: STR_NULL,
        machine_id: NUM_NULL,
        machine_name: STR_NULL,
        machine_label_index: NUM_NULL,
        log_count: NUM,
      });

      const byProcess = new Map<number | null, ProcessGroup>();
      for (; it.valid(); it.next()) {
        const upid = it.upid;
        const pid = it.pid;
        const tid = it.tid;
        const processName = it.process_name
          ? `${it.process_name} ${pid}`
          : `[unknown] ${pid ?? '?'}`;
        const threadName = it.thread_name
          ? `${it.thread_name} ${tid}`
          : `[unknown] ${tid ?? '?'}`;

        if (!byProcess.has(upid)) {
          byProcess.set(upid, {
            processName,
            pid,
            logCount: 0,
            machineId: it.machine_id ?? 0,
            machineName: it.machine_name,
            machineLabelIndex: it.machine_label_index,
            threads: [],
          });
        }
        const proc = byProcess.get(upid)!;
        proc.logCount += it.log_count;
        proc.threads.push({
          utid: it.utid,
          upid: it.upid,
          threadName,
          logCount: it.log_count,
        });
      }

      const sortedProcesses = [...byProcess.entries()].sort(
        ([, a], [, b]) => b.logCount - a.logCount,
      );

      const machineGroups = new Map<
        number,
        {node: TrackNode; utids: number[]}
      >();
      for (const [, proc] of sortedProcesses) {
        let group = machineGroups.get(proc.machineId);
        if (group === undefined) {
          const machineLabel = maybeMachineLabel(
            proc.machineLabelIndex ?? undefined,
            proc.machineName,
            numMachines,
          );
          group = {
            node: new TrackNode({
              name: `Android logs${machineLabel}`,
              isSummary: true,
              collapsed: true,
            }),
            utids: [],
          };
          machineGroups.set(proc.machineId, group);
        }
        group.utids.push(...proc.threads.map((thread) => thread.utid));
      }

      let processSortOrder = 0;
      for (const [upid, proc] of sortedProcesses) {
        const processUri = `perfetto.AndroidLog/process_${upid ?? 'unknown'}`;
        const processUtids = proc.threads.map((t) => t.utid);
        ctx.tracks.registerTrack({
          uri: processUri,
          tags: {kinds: [ANDROID_LOGS_TRACK_KIND]},
          renderer: createPerProcessLogTrack(ctx, processUri, processUtids),
        });

        const processGroup = new TrackNode({
          name: proc.processName,
          uri: processUri,
          isSummary: true,
          collapsed: true,
          sortOrder: processSortOrder++,
        });

        let threadSortOrder = 0;
        for (const thread of proc.threads) {
          const uri = `perfetto.AndroidLog/thread_${thread.utid}`;

          ctx.tracks.registerTrack({
            uri,
            tags: {
              kinds: [ANDROID_LOGS_TRACK_KIND],
              utid: thread.utid,
              upid: thread.upid ?? undefined,
            },
            renderer: createPerThreadLogTrack(ctx, uri, thread.utid),
          });

          const threadNode = new TrackNode({
            name: thread.threadName,
            uri,
            sortOrder: threadSortOrder++,
          });
          processGroup.addChildInOrder(threadNode);
        }

        machineGroups.get(proc.machineId)!.node.addChildInOrder(processGroup);
      }

      const description = () =>
        m('', [
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
      for (const [machineId, group] of machineGroups) {
        const uri = `perfetto.AndroidLog/machine_${machineId}`;
        ctx.tracks.registerTrack({
          uri,
          description,
          tags: {kinds: [ANDROID_LOGS_TRACK_KIND], machineId},
          renderer: createPerProcessLogTrack(ctx, uri, group.utids),
        });
        group.node.uri = uri;
      }

      if (machineGroups.size === 1) {
        ctx.defaultWorkspace.addChildInOrder(
          machineGroups.values().next().value!.node,
        );
      } else {
        const rootGroup = new TrackNode({
          name: 'Journal logs',
          isSummary: true,
          collapsed: true,
        });
        for (const group of machineGroups.values()) {
          rootGroup.addChildInOrder(group.node);
        }
        ctx.defaultWorkspace.addChildInOrder(rootGroup);
      }
    }

    const androidLogsTabUri = 'perfetto.AndroidLog#tab';

    // Eternal tabs should always be available even if there is nothing to show
    const filterStore = store.createSubStore(
      ['filter'],
      (x) => x as LogFilteringCriteria,
    );

    const cache = await getMachines(ctx.engine);

    ctx.tabs.registerTab({
      isEphemeral: false,
      uri: androidLogsTabUri,
      content: {
        render: () => m(LogPanel, {filterStore, cache, trace: ctx}),
        getTitle: () => 'Journal Logs',
      },
    });

    if (logCount > 0) {
      ctx.tabs.addDefaultTab(androidLogsTabUri);
    }

    ctx.commands.registerCommand({
      id: 'org.rdk.ShowJournalLogsTab',
      name: 'Show journal logs tab',
      callback: () => {
        ctx.tabs.showTab(androidLogsTabUri);
      },
    });

    ctx.search.registerSearchProvider({
      name: 'Journal logs',
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
