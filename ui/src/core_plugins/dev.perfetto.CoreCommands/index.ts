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

import {z} from 'zod';
import {copyToClipboard} from '../../base/clipboard';
import {formatTimezone, Time, time, timezoneOffsetMap} from '../../base/time';
import {exists} from '../../base/utils';
import {JsonSettingsEditor} from '../../components/json_settings_editor';
import {addQueryResultsTab} from '../../components/query_table/query_result_tab';
import {AppImpl} from '../../core/app_impl';
import {commandInvocationSchema} from '../../core/command_manager';
import {featureFlags} from '../../core/feature_flags';
import {OmniboxMode} from '../../core/omnibox_manager';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
  JsonSerialize,
  parseAppState,
  serializeAppState,
} from '../../core/state_serialization';
import {TraceImpl} from '../../core/trace_impl';
import {trackMatchesFilter} from '../../core/track_manager';
import {
  isLegacyTrace,
  openFileWithLegacyTraceViewer,
  openInOldUIWithSizeCheck,
} from '../../frontend/legacy_trace_viewer';
import {shareTrace} from '../../frontend/trace_share_utils';
import {PerfettoPlugin} from '../../public/plugin';
import {DurationPrecision, TimestampFormat} from '../../public/timeline';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {Workspace} from '../../public/workspace';
import {showModal} from '../../widgets/modal';
import {assertExists} from '../../base/logging';
import {Setting} from '../../public/settings';
import {toggleHelp} from '../../frontend/help_modal';

const QUICKSAVE_LOCALSTORAGE_KEY = 'quicksave';

const SQL_STATS = `
with first as (select started as ts from sqlstats limit 1)
select
    round((max(ended - started, 0))/1e6) as runtime_ms,
    round((started - first.ts)/1e6) as t_start_ms,
    query
from sqlstats, first
order by started desc`;

const ALL_PROCESSES_QUERY = 'select name, pid from process order by name;';

const CPU_TIME_FOR_PROCESSES = `
select
  process.name,
  sum(dur)/1e9 as cpu_sec
from sched
join thread using(utid)
join process using(upid)
group by upid
order by cpu_sec desc
limit 100;`;

const CYCLES_PER_P_STATE_PER_CPU = `
select
  cpu,
  freq,
  dur,
  sum(dur * freq)/1e6 as mcycles
from (
  select
    cpu,
    value as freq,
    lead(ts) over (partition by cpu order by ts) - ts as dur
  from counter
  inner join cpu_counter_track on counter.track_id = cpu_counter_track.id
  where name = 'cpufreq'
) group by cpu, freq
order by mcycles desc limit 32;`;

const CPU_TIME_BY_CPU_BY_PROCESS = `
select
  process.name as process,
  thread.name as thread,
  cpu,
  sum(dur) / 1e9 as cpu_sec
from sched
inner join thread using(utid)
inner join process using(upid)
group by utid, cpu
order by cpu_sec desc
limit 30;`;

const HEAP_GRAPH_BYTES_PER_TYPE = `
select
  o.upid,
  o.graph_sample_ts,
  c.name,
  sum(o.self_size) as total_self_size
from heap_graph_object o join heap_graph_class c on o.type_id = c.id
group by
 o.upid,
 o.graph_sample_ts,
 c.name
order by total_self_size desc
limit 100;`;

const SHOW_OPEN_WITH_LEGACY_UI_BUTTON = featureFlags.register({
  id: 'showOpenWithLegacyUiButton',
  name: 'Show "Open with legacy UI" button',
  description: 'Show "Open with legacy UI" button in the sidebar',
  defaultValue: false,
});

function getOrPromptForTimestamp(tsRaw: unknown): time | undefined {
  if (exists(tsRaw)) {
    if (typeof tsRaw !== 'bigint') {
      throw Error(`${tsRaw} is not a bigint`);
    }
    return Time.fromRaw(tsRaw);
  }
  // No args passed, probably run from the command palette.
  return promptForTimestamp('Enter a timestamp');
}

const macroSchema = z.record(z.array(commandInvocationSchema));
type MacroConfig = z.infer<typeof macroSchema>;

export default class CoreCommands implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CoreCommands';

  static macrosSetting: Setting<MacroConfig> | undefined = undefined;

  static onActivate(ctx: AppImpl) {
    // Register global commands (commands that are required even without a trace
    // loaded).
    ctx.commands.registerCommand({
      id: 'dev.perfetto.OpenCommandPalette',
      name: 'Open command palette',
      callback: () => ctx.omnibox.setMode(OmniboxMode.Command),
      defaultHotkey: '!Mod+Shift+P',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ShowHelp',
      name: 'Show help',
      callback: () => toggleHelp(),
      defaultHotkey: '?',
    });

    if (ctx.sidebar.enabled) {
      ctx.commands.registerCommand({
        id: 'dev.perfetto.ToggleLeftSidebar',
        name: 'Toggle left sidebar',
        callback: () => {
          ctx.sidebar.toggleVisibility();
        },
        defaultHotkey: '!Mod+B',
      });
    }

    const macroSettingsEditor = new JsonSettingsEditor<MacroConfig>({
      schema: macroSchema,
    });
    CoreCommands.macrosSetting = ctx.settings.register({
      id: 'perfetto.CoreCommands#UserDefinedMacros',
      name: 'Macros',
      description:
        'Custom command macros that execute multiple commands in sequence',
      schema: macroSchema,
      defaultValue: {},
      requiresReload: true,
      render: (setting) => macroSettingsEditor.render(setting),
    });

    const input = document.createElement('input');
    input.classList.add('trace_file');
    input.setAttribute('type', 'file');
    input.style.display = 'none';
    input.addEventListener('change', onInputElementFileSelectionChanged);
    document.body.appendChild(input);

    const OPEN_TRACE_COMMAND_ID = 'dev.perfetto.OpenTrace';
    ctx.commands.registerCommand({
      id: OPEN_TRACE_COMMAND_ID,
      name: 'Open trace file',
      callback: () => {
        delete input.dataset['useCatapultLegacyUi'];
        input.click();
      },
      defaultHotkey: '!Mod+O',
    });
    ctx.sidebar.addMenuItem({
      commandId: OPEN_TRACE_COMMAND_ID,
      section: 'trace_files',
      icon: 'folder_open',
      sortOrder: 1,
    });

    const OPEN_LEGACY_COMMAND_ID = 'dev.perfetto.OpenTraceInLegacyUi';
    ctx.commands.registerCommand({
      id: OPEN_LEGACY_COMMAND_ID,
      name: 'Open with legacy UI',
      callback: () => {
        input.dataset['useCatapultLegacyUi'] = '1';
        input.click();
      },
    });
    if (SHOW_OPEN_WITH_LEGACY_UI_BUTTON.get()) {
      ctx.sidebar.addMenuItem({
        commandId: OPEN_LEGACY_COMMAND_ID,
        section: 'trace_files',
        icon: 'filter_none',
      });
    }

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CloseTrace',
      name: 'Close trace',
      callback: () => {
        ctx.closeCurrentTrace();
      },
    });
  }

  async onTraceLoad(ctx: TraceImpl): Promise<void> {
    const app = AppImpl.instance;

    // Rgister macros from settings first.
    registerMacros(ctx, assertExists(CoreCommands.macrosSetting).get());

    // Register the macros from extras at onTraceReady (the latest time
    // possible).
    ctx.onTraceReady.addListener(async (_) => {
      // Await the promise: we've tried to be async as long as possible but
      // now we need the extras to be loaded.
      await app.extraLoadingPromise;
      registerMacros(
        ctx,
        app.extraMacros.reduce((acc, macro) => ({...acc, ...macro}), {}),
      );
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryAllProcesses',
      name: 'Run query: All processes',
      callback: () => {
        addQueryResultsTab(ctx, {
          query: ALL_PROCESSES_QUERY,
          title: 'All Processes',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryCpuTimeByProcess',
      name: 'Run query: CPU time by process',
      callback: () => {
        addQueryResultsTab(ctx, {
          query: CPU_TIME_FOR_PROCESSES,
          title: 'CPU time by process',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryCyclesByStateByCpu',
      name: 'Run query: cycles by p-state by CPU',
      callback: () => {
        addQueryResultsTab(ctx, {
          query: CYCLES_PER_P_STATE_PER_CPU,
          title: 'Cycles by p-state by CPU',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryCyclesByCpuByProcess',
      name: 'Run query: CPU Time by CPU by process',
      callback: () => {
        addQueryResultsTab(ctx, {
          query: CPU_TIME_BY_CPU_BY_PROCESS,
          title: 'CPU time by CPU by process',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryHeapGraphBytesPerType',
      name: 'Run query: heap graph bytes per type',
      callback: () => {
        addQueryResultsTab(ctx, {
          query: HEAP_GRAPH_BYTES_PER_TYPE,
          title: 'Heap graph bytes per type',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.DebugSqlPerformance',
      name: 'Debug SQL performance',
      callback: () => {
        addQueryResultsTab(ctx, {
          query: SQL_STATS,
          title: 'Recent SQL queries',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.UnpinAllTracks',
      name: 'Unpin all pinned tracks',
      callback: () => {
        const workspace = ctx.currentWorkspace;
        workspace.pinnedTracks.forEach((t) => workspace.unpinTrack(t));
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ExpandAllGroups',
      name: 'Expand all track groups',
      callback: () => {
        ctx.currentWorkspace.flatTracks.forEach((track) => track.expand());
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CollapseAllGroups',
      name: 'Collapse all track groups',
      callback: () => {
        ctx.currentWorkspace.flatTracks.forEach((track) => track.collapse());
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.PanToTimestamp',
      name: 'Pan to timestamp',
      callback: (tsRaw: unknown) => {
        const ts = getOrPromptForTimestamp(tsRaw);
        if (ts !== undefined) {
          ctx.timeline.panIntoView(ts, {align: 'center'});
        }
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.MarkTimestamp',
      name: 'Mark timestamp',
      callback: (tsRaw: unknown) => {
        const ts = getOrPromptForTimestamp(tsRaw);
        if (ts !== undefined) {
          ctx.notes.addNote({
            timestamp: ts,
          });
        }
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ShowCurrentSelectionTab',
      name: 'Show current selection tab',
      callback: () => {
        ctx.tabs.showTab('current_selection');
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CreateWorkspace',
      name: 'Create new empty workspace',
      callback: async (rawName: unknown) => {
        const workspaces = ctx.workspaces;
        if (workspaces === undefined) return; // No trace loaded.
        const name =
          typeof rawName === 'string'
            ? rawName
            : await ctx.omnibox.prompt('Give it a name...');
        if (name === undefined || name === '') return;
        workspaces.createEmptyWorkspace(name);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CreateWorkspaceAndSwitch',
      name: 'Create new empty workspace and switch to it',
      callback: async (rawName: unknown) => {
        const workspaces = ctx.workspaces;
        if (workspaces === undefined) return; // No trace loaded.
        const name =
          typeof rawName === 'string'
            ? rawName
            : await ctx.omnibox.prompt('Give it a name...');
        if (name === undefined || name === '') return;
        workspaces.switchWorkspace(workspaces.createEmptyWorkspace(name));
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SwitchWorkspace',
      name: 'Switch to workspace',
      callback: async (rawName: unknown) => {
        const workspaces = ctx.workspaces;
        if (workspaces === undefined) return; // No trace loaded.
        const workspace =
          workspaces.all.find((x) => x.title === rawName) ??
          (await ctx.omnibox.prompt('Choose a workspace...', {
            values: workspaces.all,
            getName: (ws) => ws.title,
          }));
        if (workspace) {
          workspaces.switchWorkspace(workspace);
        }
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SetTimestampFormat',
      name: 'Set timestamp and duration format',
      callback: async () => {
        const TF = TimestampFormat;
        const timeZone = formatTimezone(ctx.traceInfo.tzOffMin);
        const result = await ctx.omnibox.prompt('Select format...', {
          values: [
            {format: TF.Timecode, name: 'Timecode'},
            {format: TF.UTC, name: 'Realtime (UTC)'},

            {format: TF.TraceTz, name: `Realtime (Trace TZ - ${timeZone})`},
            {format: TF.Seconds, name: 'Seconds'},
            {format: TF.Milliseconds, name: 'Milliseconds'},
            {format: TF.Microseconds, name: 'Microseconds'},
            {format: TF.TraceNs, name: 'Trace nanoseconds'},
            {
              format: TF.TraceNsLocale,
              name: 'Trace nanoseconds (with locale-specific formatting)',
            },
            {format: TF.CustomTimezone, name: 'Custom Timezone'},
          ],
          getName: (x) => x.name,
        });
        if (!result) return;

        if (result.format === TF.CustomTimezone) {
          const result = await ctx.omnibox.prompt('Select format...', {
            values: Object.entries(timezoneOffsetMap),
            getName: ([key]) => key,
          });

          if (!result) return;
          ctx.timeline.timezoneOverride.set(result[0]);
        }

        ctx.timeline.timestampFormat = result.format;
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SetDurationPrecision',
      name: 'Set duration precision',
      callback: async () => {
        const DF = DurationPrecision;
        const result = await ctx.omnibox.prompt(
          'Select duration precision mode...',
          {
            values: [
              {format: DF.Full, name: 'Full'},
              {format: DF.HumanReadable, name: 'Human readable'},
            ],
            getName: (x) => x.name,
          },
        );
        result && (ctx.timeline.durationPrecision = result.format);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.TogglePerformanceMetrics',
      name: 'Toggle performance metrics',
      callback: () => (ctx.perfDebugging.enabled = !ctx.perfDebugging.enabled),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ShareTrace',
      name: 'Share trace',
      callback: () => shareTrace(ctx),
    });
    ctx.commands.registerCommand({
      id: 'dev.perfetto.SearchNext',
      name: 'Go to next search result',
      callback: () => {
        ctx.search.stepForward();
      },
      defaultHotkey: 'Enter',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SearchPrev',
      name: 'Go to previous search result',
      callback: () => {
        ctx.search.stepBackwards();
      },
      defaultHotkey: 'Shift+Enter',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SwitchToQueryMode',
      name: 'Switch to query mode',
      callback: () => ctx.omnibox.setMode(OmniboxMode.Query),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQuery',
      name: 'Runs an SQL query',
      callback: async (rawSql: unknown) => {
        const query =
          typeof rawSql === 'string'
            ? rawSql
            : await ctx.omnibox.prompt('Enter SQL...');
        if (!query) {
          return;
        }
        await ctx.engine.query(query);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.RunQueryAndShowTab',
      name: 'Runs an SQL query and opens results in a tab',
      callback: async (rawSql: unknown) => {
        const query =
          typeof rawSql === 'string'
            ? rawSql
            : await ctx.omnibox.prompt('Enter SQL...');
        if (!query) {
          return;
        }
        addQueryResultsTab(ctx, {
          query,
          title: 'Command Query',
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SwitchToSearchMode',
      name: 'Switch to search mode',
      callback: () => ctx.omnibox.setMode(OmniboxMode.Search),
      defaultHotkey: '/',
    });
    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopyTimeWindow',
      name: `Copy selected time window to clipboard`,
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        const query = `ts >= ${window.start} and ts < ${window.end}`;
        copyToClipboard(query);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.FocusSelection',
      name: 'Focus current selection',
      callback: () => ctx.selection.scrollToSelection('focus'),
      defaultHotkey: 'F',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.Deselect',
      name: 'Deselect',
      callback: () => {
        ctx.selection.clearSelection();
      },
      defaultHotkey: 'Escape',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.NextFlow',
      name: 'Next flow',
      callback: () => ctx.flows.focusOtherFlow('Forward'),
      defaultHotkey: 'Mod+]',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.PrevFlow',
      name: 'Prev flow',
      callback: () => ctx.flows.focusOtherFlow('Backward'),
      defaultHotkey: 'Mod+[',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.MoveNextFlow',
      name: 'Move next flow',
      callback: () => ctx.flows.moveByFocusedFlow('Forward'),
      defaultHotkey: ']',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.MovePrevFlow',
      name: 'Move prev flow',
      callback: () => ctx.flows.moveByFocusedFlow('Backward'),
      defaultHotkey: '[',
    });

    // Provides a test bed for resolving events using a SQL table name and ID
    // which is used in deep-linking, amongst other places.
    ctx.commands.registerCommand({
      id: 'dev.perfetto.SelectEventByTableNameAndId',
      name: 'Select event by table name and ID',
      callback: async () => {
        const rootTableName = await ctx.omnibox.prompt('Enter table name');
        if (rootTableName === undefined) return;

        const id = await ctx.omnibox.prompt('Enter ID');
        if (id === undefined) return;

        const num = Number(id);
        if (!isFinite(num)) return; // Rules out NaN or +-Infinity

        ctx.selection.selectSqlEvent(rootTableName, num, {
          scrollToSelection: true,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.SelectAll',
      name: 'Select all',
      callback: () => {
        // This is a dual state command:
        // - If one ore more tracks are already area selected, expand the time
        //   range to include the entire trace, but keep the selection on just
        //   these tracks.
        // - If nothing is selected, or all selected tracks are entirely
        //   selected, then select the entire trace. This allows double tapping
        //   Ctrl+A to select the entire track, then select the entire trace.
        let tracksToSelect: ReadonlyArray<string>;
        const selection = ctx.selection.selection;
        if (selection.kind === 'area') {
          // Something is already selected, let's see if it covers the entire
          // span of the trace or not
          const coversEntireTimeRange =
            ctx.traceInfo.start === selection.start &&
            ctx.traceInfo.end === selection.end;
          if (!coversEntireTimeRange) {
            // If the current selection is an area which does not cover the
            // entire time range, preserve the list of selected tracks and
            // expand the time range.
            tracksToSelect = selection.trackUris;
          } else {
            // If the entire time range is already covered, update the selection
            // to cover all tracks.
            tracksToSelect = ctx.currentWorkspace.flatTracks
              .map((t) => t.uri)
              .filter((uri) => uri !== undefined);
          }
        } else {
          // If the current selection is not an area, select all.
          tracksToSelect = ctx.currentWorkspace.flatTracks
            .map((t) => t.uri)
            .filter((uri) => uri !== undefined);
        }
        const {start, end} = ctx.traceInfo;
        ctx.selection.selectArea({
          start,
          end,
          trackUris: tracksToSelect,
        });
      },
      defaultHotkey: 'Mod+A',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ConvertSelectionToArea',
      name: 'Convert selection to area selection',
      callback: () => {
        const selection = ctx.selection.selection;
        const range = ctx.selection.getTimeSpanOfSelection();
        if (selection.kind === 'track_event' && range) {
          ctx.selection.selectArea({
            start: range.start,
            end: range.end,
            trackUris: [selection.trackUri],
          });
        }
      },
      defaultHotkey: 'R',
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.ToggleDrawer',
      name: 'Toggle drawer',
      defaultHotkey: 'Q',
      callback: () => ctx.tabs.toggleTabPanelVisibility(),
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopyPinnedToWorkspace',
      name: 'Copy pinned tracks to workspace',
      callback: async () => {
        const pinnedTracks = ctx.currentWorkspace.pinnedTracks;
        if (!pinnedTracks.length) {
          window.alert('No pinned tracks to copy');
          return;
        }

        const ws = await this.selectWorkspace(ctx, 'Pinned tracks');
        if (!ws) return;

        for (const pinnedTrack of pinnedTracks) {
          const clone = pinnedTrack.clone();
          ws.addChildLast(clone);
        }
        ctx.workspaces.switchWorkspace(ws);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopyFilteredToWorkspace',
      name: 'Copy filtered tracks to workspace',
      callback: async () => {
        // Copies all filtered tracks as a flat list to a new workspace. This
        // means parents are not included.
        const tracks = ctx.currentWorkspace.flatTracks.filter((track) =>
          trackMatchesFilter(ctx, track),
        );

        if (!tracks.length) {
          window.alert('No filtered tracks to copy');
          return;
        }

        const ws = await this.selectWorkspace(ctx, 'Filtered tracks');
        if (!ws) return;

        for (const track of tracks) {
          const clone = track.clone();
          ws.addChildLast(clone);
        }
        ctx.workspaces.switchWorkspace(ws);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CopySelectedTracksToWorkspace',
      name: 'Copy selected tracks to workspace',
      callback: async () => {
        const selection = ctx.selection.selection;

        if (selection.kind !== 'area' || selection.trackUris.length === 0) {
          window.alert('No selected tracks to copy');
          return;
        }

        const workspace = await this.selectWorkspace(ctx);
        if (!workspace) return;

        for (const uri of selection.trackUris) {
          const node = ctx.currentWorkspace.getTrackByUri(uri);
          if (!node) continue;
          const newNode = node.clone();
          workspace.addChildLast(newNode);
        }
        ctx.workspaces.switchWorkspace(workspace);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.Quicksave',
      name: 'Quicksave UI state to localStorage',
      callback: () => {
        const state = serializeAppState(ctx);
        const json = JsonSerialize(state);
        localStorage.setItem(QUICKSAVE_LOCALSTORAGE_KEY, json);
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.Quickload',
      name: 'Quickload UI state from the localStorage',
      callback: () => {
        const json = localStorage.getItem(QUICKSAVE_LOCALSTORAGE_KEY);
        if (json === null) {
          showModal({
            title: 'Nothing saved in the quicksave slot',
            buttons: [{text: 'Dismiss'}],
          });
          return;
        }
        const parsed = JSON.parse(json);
        const state = parseAppState(parsed);
        if (state.ok) {
          deserializeAppStatePhase1(state.value, ctx);
          deserializeAppStatePhase2(state.value, ctx);
        }
      },
    });

    ctx.commands.registerCommand({
      id: `dev.perfetto.RestoreDefaults`,
      name: 'Reset all flags back to default values',
      callback: () => {
        featureFlags.resetAll();
        window.location.reload();
      },
    });
  }

  // Selects a workspace or creates a new one.
  private async selectWorkspace(
    trace: TraceImpl,
    newWorkspaceName = 'Untitled workspace',
  ): Promise<Workspace | undefined> {
    const options = trace.workspaces.all
      .filter((ws) => ws.userEditable)
      .map((ws) => ({title: ws.title, fn: () => ws}))
      .concat([
        {
          title: 'New workspace...',
          fn: () => trace.workspaces.createEmptyWorkspace(newWorkspaceName),
        },
      ]);

    const result = await trace.omnibox.prompt('Select a workspace...', {
      values: options,
      getName: (ws) => ws.title,
    });

    if (!result) return undefined;
    return result.fn();
  }
}

function promptForTimestamp(message: string): time | undefined {
  const tsStr = window.prompt(message);
  if (tsStr !== null) {
    try {
      return Time.fromRaw(BigInt(tsStr));
    } catch {
      window.alert(`${tsStr} is not an integer`);
    }
  }
  return undefined;
}

function onInputElementFileSelectionChanged(e: Event) {
  if (!(e.target instanceof HTMLInputElement)) {
    throw new Error('Not an input element');
  }
  if (!e.target.files) return;
  const file = e.target.files[0];
  // Reset the value so onchange will be fired with the same file.
  e.target.value = '';

  if (e.target.dataset['useCatapultLegacyUi'] === '1') {
    openWithLegacyUi(file);
    return;
  }

  AppImpl.instance.analytics.logEvent('Trace Actions', 'Open trace from file');
  AppImpl.instance.openTraceFromFile(file);
}

async function openWithLegacyUi(file: File) {
  // Switch back to the old catapult UI.
  AppImpl.instance.analytics.logEvent(
    'Trace Actions',
    'Open trace in Legacy UI',
  );
  if (await isLegacyTrace(file)) {
    return await openFileWithLegacyTraceViewer(file);
  }
  return await openInOldUIWithSizeCheck(file);
}

function registerMacros(trace: TraceImpl, config: MacroConfig) {
  for (const [macroName, commands] of Object.entries(config)) {
    trace.commands.registerCommand({
      id: `dev.perfetto.UserMacro.${macroName}`,
      name: macroName,
      callback: async () => {
        // Macros could run multiple commands, some of which might prompt the
        // user in an optional way. But macros should be self-contained
        // so we disable prompts during their execution.
        using _ = trace.omnibox.disablePrompts();
        for (const command of commands) {
          await trace.commands.runCommand(command.id, ...command.args);
        }
      },
    });
  }
}
