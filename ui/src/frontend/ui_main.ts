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

import m from 'mithril';
import {copyToClipboard} from '../base/clipboard';
import {findRef} from '../base/dom_utils';
import {FuzzyFinder} from '../base/fuzzy';
import {assertExists, assertUnreachable} from '../base/logging';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {Command} from '../public/command';
import {HotkeyConfig, HotkeyContext} from '../widgets/hotkey_context';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {maybeRenderFullscreenModalDialog, showModal} from '../widgets/modal';
import {CookieConsent} from '../core/cookie_consent';
import {toggleHelp} from './help_modal';
import {Omnibox, OmniboxOption} from './omnibox';
import {addQueryResultsTab} from '../components/query_table/query_result_tab';
import {Sidebar} from './sidebar';
import {Topbar} from './topbar';
import {shareTrace} from './trace_share_utils';
import {OmniboxMode} from '../core/omnibox_manager';
import {DisposableStack} from '../base/disposable_stack';
import {Spinner} from '../widgets/spinner';
import {TraceImpl} from '../core/trace_impl';
import {AppImpl} from '../core/app_impl';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../public/utils';
import {DurationPrecision, TimestampFormat} from '../public/timeline';
import {Workspace} from '../public/workspace';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
  JsonSerialize,
  parseAppState,
  serializeAppState,
} from '../core/state_serialization';
import {featureFlags} from '../core/feature_flags';
import {trackMatchesFilter} from '../core/track_manager';
import {renderStatusBar} from './statusbar';
import {formatTimezone, timezoneOffsetMap} from '../base/time';
import {LinearProgress} from '../widgets/linear_progress';
import {taskTracker} from './task_tracker';

const showStatusBarFlag = featureFlags.register({
  id: 'Enable status bar',
  description: 'Enable status bar at the bottom of the window',
  defaultValue: true,
});

const QUICKSAVE_LOCALSTORAGE_KEY = 'quicksave';
const OMNIBOX_INPUT_REF = 'omnibox';

// This wrapper creates a new instance of UiMainPerTrace for each new trace
// loaded (including the case of no trace at the beginning).
export class UiMain implements m.ClassComponent {
  view() {
    const currentTraceId = AppImpl.instance.trace?.engine.engineId ?? '';
    return [m(UiMainPerTrace, {key: currentTraceId})];
  }
}

// This components gets destroyed and recreated every time the current trace
// changes. Note that in the beginning the current trace is undefined.
export class UiMainPerTrace implements m.ClassComponent {
  // NOTE: this should NOT need to be an AsyncDisposableStack. If you feel the
  // need of making it async because you want to clean up SQL resources, that
  // will cause bugs (see comments in oncreate()).
  private trash = new DisposableStack();
  private omniboxInputEl?: HTMLInputElement;
  private recentCommands: string[] = [];
  private trace?: TraceImpl;

  // This function is invoked once per trace.
  constructor() {
    const app = AppImpl.instance;
    const trace = app.trace;
    this.trace = trace;

    // Register global commands (commands that are useful even without a trace
    // loaded).
    const globalCmds: Command[] = [
      {
        id: 'perfetto.OpenCommandPalette',
        name: 'Open command palette',
        callback: () => app.omnibox.setMode(OmniboxMode.Command),
        defaultHotkey: '!Mod+Shift+P',
      },

      {
        id: 'perfetto.ShowHelp',
        name: 'Show help',
        callback: () => toggleHelp(),
        defaultHotkey: '?',
      },
    ];
    globalCmds.forEach((cmd) => {
      this.trash.use(app.commands.registerCommand(cmd));
    });

    // When the UI loads there is no trace. There is no point registering
    // commands or anything in this state as they will be useless.
    if (trace === undefined) return;
    document.title = `${trace.traceInfo.traceTitle || 'Trace'} - Perfetto UI`;
    this.maybeShowJsonWarning();

    const cmds: Command[] = [
      {
        id: 'perfetto.SetTimestampFormat',
        name: 'Set timestamp and duration format',
        callback: async () => {
          const TF = TimestampFormat;
          const timeZone = formatTimezone(trace.traceInfo.tzOffMin);
          const result = await app.omnibox.prompt('Select format...', {
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
            const result = await app.omnibox.prompt('Select format...', {
              values: Object.entries(timezoneOffsetMap),
              getName: ([key]) => key,
            });

            if (!result) return;
            trace.timeline.timezoneOverride.set(result[0]);
          }

          trace.timeline.timestampFormat = result.format;
        },
      },
      {
        id: 'perfetto.SetDurationPrecision',
        name: 'Set duration precision',
        callback: async () => {
          const DF = DurationPrecision;
          const result = await app.omnibox.prompt(
            'Select duration precision mode...',
            {
              values: [
                {format: DF.Full, name: 'Full'},
                {format: DF.HumanReadable, name: 'Human readable'},
              ],
              getName: (x) => x.name,
            },
          );
          result && (trace.timeline.durationPrecision = result.format);
        },
      },
      {
        id: 'perfetto.TogglePerformanceMetrics',
        name: 'Toggle performance metrics',
        callback: () =>
          (app.perfDebugging.enabled = !app.perfDebugging.enabled),
      },
      {
        id: 'perfetto.ShareTrace',
        name: 'Share trace',
        callback: () => shareTrace(trace),
      },
      {
        id: 'perfetto.SearchNext',
        name: 'Go to next search result',
        callback: () => {
          trace.search.stepForward();
        },
        defaultHotkey: 'Enter',
      },
      {
        id: 'perfetto.SearchPrev',
        name: 'Go to previous search result',
        callback: () => {
          trace.search.stepBackwards();
        },
        defaultHotkey: 'Shift+Enter',
      },
      {
        id: 'perfetto.RunQuery',
        name: 'Run query',
        callback: () => trace.omnibox.setMode(OmniboxMode.Query),
      },
      {
        id: 'perfetto.Search',
        name: 'Search',
        callback: () => trace.omnibox.setMode(OmniboxMode.Search),
        defaultHotkey: '/',
      },
      {
        id: 'perfetto.CopyTimeWindow',
        name: `Copy selected time window to clipboard`,
        callback: async () => {
          const window = await getTimeSpanOfSelectionOrVisibleWindow(trace);
          const query = `ts >= ${window.start} and ts < ${window.end}`;
          copyToClipboard(query);
        },
      },
      {
        id: 'perfetto.FocusSelection',
        name: 'Focus current selection',
        callback: () => trace.selection.scrollToSelection(),
        defaultHotkey: 'F',
      },
      {
        id: 'perfetto.Deselect',
        name: 'Deselect',
        callback: () => {
          trace.selection.clearSelection();
        },
        defaultHotkey: 'Escape',
      },
      {
        id: 'perfetto.NextFlow',
        name: 'Next flow',
        callback: () => trace.flows.focusOtherFlow('Forward'),
        defaultHotkey: 'Mod+]',
      },
      {
        id: 'perfetto.PrevFlow',
        name: 'Prev flow',
        callback: () => trace.flows.focusOtherFlow('Backward'),
        defaultHotkey: 'Mod+[',
      },
      {
        id: 'perfetto.MoveNextFlow',
        name: 'Move next flow',
        callback: () => trace.flows.moveByFocusedFlow('Forward'),
        defaultHotkey: ']',
      },
      {
        id: 'perfetto.MovePrevFlow',
        name: 'Move prev flow',
        callback: () => trace.flows.moveByFocusedFlow('Backward'),
        defaultHotkey: '[',
      },

      // Provides a test bed for resolving events using a SQL table name and ID
      // which is used in deep-linking, amongst other places.
      {
        id: 'perfetto.SelectEventByTableNameAndId',
        name: 'Select event by table name and ID',
        callback: async () => {
          const rootTableName = await trace.omnibox.prompt('Enter table name');
          if (rootTableName === undefined) return;

          const id = await trace.omnibox.prompt('Enter ID');
          if (id === undefined) return;

          const num = Number(id);
          if (!isFinite(num)) return; // Rules out NaN or +-Infinity

          trace.selection.selectSqlEvent(rootTableName, num, {
            scrollToSelection: true,
          });
        },
      },
      {
        id: 'perfetto.SelectAll',
        name: 'Select all',
        callback: () => {
          // This is a dual state command:
          // - If one ore more tracks are already area selected, expand the time
          //   range to include the entire trace, but keep the selection on just
          //   these tracks.
          // - If nothing is selected, or all selected tracks are entirely
          //   selected, then select the entire trace. This allows double tapping
          //   Ctrl+A to select the entire track, then select the entire trace.
          let tracksToSelect: string[];
          const selection = trace.selection.selection;
          if (selection.kind === 'area') {
            // Something is already selected, let's see if it covers the entire
            // span of the trace or not
            const coversEntireTimeRange =
              trace.traceInfo.start === selection.start &&
              trace.traceInfo.end === selection.end;
            if (!coversEntireTimeRange) {
              // If the current selection is an area which does not cover the
              // entire time range, preserve the list of selected tracks and
              // expand the time range.
              tracksToSelect = selection.trackUris;
            } else {
              // If the entire time range is already covered, update the selection
              // to cover all tracks.
              tracksToSelect = trace.workspace.flatTracks
                .map((t) => t.uri)
                .filter((uri) => uri !== undefined);
            }
          } else {
            // If the current selection is not an area, select all.
            tracksToSelect = trace.workspace.flatTracks
              .map((t) => t.uri)
              .filter((uri) => uri !== undefined);
          }
          const {start, end} = trace.traceInfo;
          trace.selection.selectArea({
            start,
            end,
            trackUris: tracksToSelect,
          });
        },
        defaultHotkey: 'Mod+A',
      },
      {
        id: 'perfetto.ConvertSelectionToArea',
        name: 'Convert selection to area selection',
        callback: () => {
          const selection = trace.selection.selection;
          const range = trace.selection.getTimeSpanOfSelection();
          if (selection.kind === 'track_event' && range) {
            trace.selection.selectArea({
              start: range.start,
              end: range.end,
              trackUris: [selection.trackUri],
            });
          }
        },
        defaultHotkey: 'R',
      },
      {
        id: 'perfetto.ToggleDrawer',
        name: 'Toggle drawer',
        defaultHotkey: 'Q',
        callback: () => trace.tabs.toggleTabPanelVisibility(),
      },
      {
        id: 'perfetto.CopyPinnedToWorkspace',
        name: 'Copy pinned tracks to workspace',
        callback: async () => {
          const pinnedTracks = trace.workspace.pinnedTracks;
          if (!pinnedTracks.length) {
            window.alert('No pinned tracks to copy');
            return;
          }

          const ws = await this.selectWorkspace(trace, 'Pinned tracks');
          if (!ws) return;

          for (const pinnedTrack of pinnedTracks) {
            const clone = pinnedTrack.clone();
            ws.addChildLast(clone);
          }
          trace.workspaces.switchWorkspace(ws);
        },
      },
      {
        id: 'perfetto.CopyFilteredToWorkspace',
        name: 'Copy filtered tracks to workspace',
        callback: async () => {
          // Copies all filtered tracks as a flat list to a new workspace. This
          // means parents are not included.
          const tracks = trace.workspace.flatTracks.filter((track) =>
            trackMatchesFilter(trace, track),
          );

          if (!tracks.length) {
            window.alert('No filtered tracks to copy');
            return;
          }

          const ws = await this.selectWorkspace(trace, 'Filtered tracks');
          if (!ws) return;

          for (const track of tracks) {
            const clone = track.clone();
            ws.addChildLast(clone);
          }
          trace.workspaces.switchWorkspace(ws);
        },
      },
      {
        id: 'perfetto.CopySelectedTracksToWorkspace',
        name: 'Copy selected tracks to workspace',
        callback: async () => {
          const selection = trace.selection.selection;

          if (selection.kind !== 'area' || selection.trackUris.length === 0) {
            window.alert('No selected tracks to copy');
            return;
          }

          const workspace = await this.selectWorkspace(trace);
          if (!workspace) return;

          for (const uri of selection.trackUris) {
            const node = trace.workspace.getTrackByUri(uri);
            if (!node) continue;
            const newNode = node.clone();
            workspace.addChildLast(newNode);
          }
          trace.workspaces.switchWorkspace(workspace);
        },
      },
      {
        id: 'perfetto.Quicksave',
        name: 'Quicksave UI state to localStorage',
        callback: () => {
          const state = serializeAppState(trace);
          const json = JsonSerialize(state);
          localStorage.setItem(QUICKSAVE_LOCALSTORAGE_KEY, json);
        },
      },
      {
        id: 'perfetto.Quickload',
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
          if (state.success) {
            deserializeAppStatePhase1(state.data, trace);
            deserializeAppStatePhase2(state.data, trace);
          }
        },
      },
      {
        id: `${app.pluginId}#RestoreDefaults`,
        name: 'Reset all flags back to default values',
        callback: () => {
          featureFlags.resetAll();
          window.location.reload();
        },
      },
    ];

    // Register each command with the command manager
    cmds.forEach((cmd) => {
      this.trash.use(trace.commands.registerCommand(cmd));
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

  private renderOmnibox(): m.Children {
    const omnibox = AppImpl.instance.omnibox;
    const omniboxMode = omnibox.mode;
    const statusMessage = omnibox.statusMessage;
    if (statusMessage !== undefined) {
      return m(
        `.omnibox.message-mode`,
        m(`input[readonly][disabled][ref=omnibox]`, {
          value: '',
          placeholder: statusMessage,
        }),
      );
    } else if (omniboxMode === OmniboxMode.Command) {
      return this.renderCommandOmnibox();
    } else if (omniboxMode === OmniboxMode.Prompt) {
      return this.renderPromptOmnibox();
    } else if (omniboxMode === OmniboxMode.Query) {
      return this.renderQueryOmnibox();
    } else if (omniboxMode === OmniboxMode.Search) {
      return this.renderSearchOmnibox();
    } else {
      assertUnreachable(omniboxMode);
    }
  }

  renderPromptOmnibox(): m.Children {
    const omnibox = AppImpl.instance.omnibox;
    const prompt = assertExists(omnibox.pendingPrompt);

    let options: OmniboxOption[] | undefined = undefined;

    if (prompt.options) {
      const fuzzy = new FuzzyFinder(
        prompt.options,
        ({displayName}) => displayName,
      );
      const result = fuzzy.find(omnibox.text);
      options = result.map((result) => {
        return {
          key: result.item.key,
          displayName: result.segments,
        };
      });
    }

    return m(Omnibox, {
      value: omnibox.text,
      placeholder: prompt.text,
      inputRef: OMNIBOX_INPUT_REF,
      extraClasses: 'prompt-mode',
      closeOnOutsideClick: true,
      options,
      selectedOptionIndex: omnibox.selectionIndex,
      onSelectedOptionChanged: (index) => {
        omnibox.setSelectionIndex(index);
      },
      onInput: (value) => {
        omnibox.setText(value);
        omnibox.setSelectionIndex(0);
      },
      onSubmit: (value, _alt) => {
        omnibox.resolvePrompt(value);
      },
      onClose: () => {
        omnibox.rejectPrompt();
      },
    });
  }

  renderCommandOmnibox(): m.Children {
    // Fuzzy-filter commands by the filter string.
    const {commands, omnibox} = AppImpl.instance;
    const filteredCmds = commands.fuzzyFilterCommands(omnibox.text);

    // Create an array of commands with attached heuristics from the recent
    // command register.
    const commandsWithHeuristics = filteredCmds.map((cmd) => {
      return {
        recentsIndex: this.recentCommands.findIndex((id) => id === cmd.id),
        cmd,
      };
    });

    // Sort recentsIndex first
    const sorted = commandsWithHeuristics.sort((a, b) => {
      if (b.recentsIndex === a.recentsIndex) {
        // If recentsIndex is the same, retain original sort order
        return 0;
      } else {
        return b.recentsIndex - a.recentsIndex;
      }
    });

    const options = sorted.map(({recentsIndex, cmd}): OmniboxOption => {
      const {segments, id, defaultHotkey} = cmd;
      return {
        key: id,
        displayName: segments,
        tag: recentsIndex !== -1 ? 'recently used' : undefined,
        rightContent: defaultHotkey && m(HotkeyGlyphs, {hotkey: defaultHotkey}),
      };
    });

    return m(Omnibox, {
      value: omnibox.text,
      placeholder: 'Filter commands...',
      inputRef: OMNIBOX_INPUT_REF,
      extraClasses: 'command-mode',
      options,
      closeOnSubmit: true,
      closeOnOutsideClick: true,
      selectedOptionIndex: omnibox.selectionIndex,
      onSelectedOptionChanged: (index) => {
        omnibox.setSelectionIndex(index);
      },
      onInput: (value) => {
        omnibox.setText(value);
        omnibox.setSelectionIndex(0);
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        omnibox.reset();
      },
      onSubmit: (key: string) => {
        this.addRecentCommand(key);
        commands.runCommand(key);
      },
      onGoBack: () => {
        omnibox.reset();
      },
    });
  }

  private addRecentCommand(id: string): void {
    this.recentCommands = this.recentCommands.filter((x) => x !== id);
    this.recentCommands.push(id);
    while (this.recentCommands.length > 6) {
      this.recentCommands.shift();
    }
  }

  renderQueryOmnibox(): m.Children {
    const ph = 'e.g. select * from sched left join thread using(utid) limit 10';
    return m(Omnibox, {
      value: AppImpl.instance.omnibox.text,
      placeholder: ph,
      inputRef: OMNIBOX_INPUT_REF,
      extraClasses: 'query-mode',

      onInput: (value) => {
        AppImpl.instance.omnibox.setText(value);
      },
      onSubmit: (query, alt) => {
        const config = {
          query: undoCommonChatAppReplacements(query),
          title: alt ? 'Pinned query' : 'Omnibox query',
        };
        const tag = alt ? undefined : 'omnibox_query';
        if (this.trace === undefined) return; // No trace loaded
        addQueryResultsTab(this.trace, config, tag);
      },
      onClose: () => {
        AppImpl.instance.omnibox.setText('');
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        AppImpl.instance.omnibox.reset();
      },
      onGoBack: () => {
        AppImpl.instance.omnibox.reset();
      },
    });
  }

  renderSearchOmnibox(): m.Children {
    return m(Omnibox, {
      value: AppImpl.instance.omnibox.text,
      placeholder: "Search or type '>' for commands or ':' for SQL mode",
      inputRef: OMNIBOX_INPUT_REF,
      onInput: (value, _prev) => {
        if (value === '>') {
          AppImpl.instance.omnibox.setMode(OmniboxMode.Command);
          return;
        } else if (value === ':') {
          AppImpl.instance.omnibox.setMode(OmniboxMode.Query);
          return;
        }
        AppImpl.instance.omnibox.setText(value);
        if (this.trace === undefined) return; // No trace loaded.
        if (value.length >= 4) {
          this.trace.search.search(value);
        } else {
          this.trace.search.reset();
        }
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (value, _mod, shift) => {
        if (this.trace === undefined) return; // No trace loaded.
        this.trace.search.search(value);
        if (shift) {
          this.trace.search.stepBackwards();
        } else {
          this.trace.search.stepForward();
        }
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      rightContent: this.renderStepThrough(),
    });
  }

  private renderStepThrough() {
    const children = [];
    const results = this.trace?.search.searchResults;
    if (this.trace?.search.searchInProgress) {
      children.push(m('.current', m(Spinner)));
    } else if (results !== undefined) {
      const searchMgr = assertExists(this.trace).search;
      const index = searchMgr.resultIndex;
      const total = results.totalResults ?? 0;
      children.push(
        m('.current', `${total === 0 ? '0 / 0' : `${index + 1} / ${total}`}`),
        m(
          'button',
          {
            onclick: () => searchMgr.stepBackwards(),
          },
          m('i.material-icons.left', 'keyboard_arrow_left'),
        ),
        m(
          'button',
          {
            onclick: () => searchMgr.stepForward(),
          },
          m('i.material-icons.right', 'keyboard_arrow_right'),
        ),
      );
    }
    return m('.stepthrough', children);
  }

  oncreate(vnode: m.VnodeDOM) {
    this.updateOmniboxInputRef(vnode.dom);
    this.maybeFocusOmnibar();
  }

  view(): m.Children {
    const app = AppImpl.instance;
    const hotkeys: HotkeyConfig[] = [];
    for (const {id, defaultHotkey} of app.commands.commands) {
      if (defaultHotkey) {
        hotkeys.push({
          callback: () => app.commands.runCommand(id),
          hotkey: defaultHotkey,
        });
      }
    }

    const isSomethingLoading =
      AppImpl.instance.isLoadingTrace ||
      (this.trace?.engine.numRequestsPending ?? 0) > 0 ||
      taskTracker.hasPendingTasks();

    return m(
      HotkeyContext,
      {hotkeys},
      m(
        'main.pf-ui-main',
        m(Sidebar, {trace: this.trace}),
        m(Topbar, {
          omnibox: this.renderOmnibox(),
          trace: this.trace,
        }),
        m(LinearProgress, {
          className: 'pf-ui-main__loading',
          state: isSomethingLoading ? 'indeterminate' : 'none',
        }),
        app.pages.renderPageForCurrentRoute(),
        m(CookieConsent),
        maybeRenderFullscreenModalDialog(),
        showStatusBarFlag.get() && renderStatusBar(app.trace),
        app.perfDebugging.renderPerfStats(),
      ),
    );
  }

  onupdate({dom}: m.VnodeDOM) {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
  }

  onremove(_: m.VnodeDOM) {
    this.omniboxInputEl = undefined;

    // NOTE: if this becomes ever an asyncDispose(), then the promise needs to
    // be returned to onbeforeremove, so mithril delays the removal until
    // the promise is resolved, but then also the UiMain wrapper needs to be
    // more complex to linearize the destruction of the old instane with the
    // creation of the new one, without overlaps.
    // However, we should not add disposables that issue cleanup queries on the
    // Engine. Doing so is: (1) useless: we throw away the whole wasm instance
    // on each trace load, so what's the point of deleting tables from a TP
    // instance that is going to be destroyed?; (2) harmful: we don't have
    // precise linearization with the wasm teardown, so we might end up awaiting
    // forever for the asyncDispose() because the query will never run.
    this.trash.dispose();
  }

  private updateOmniboxInputRef(dom: Element): void {
    const el = findRef(dom, OMNIBOX_INPUT_REF);
    if (el && el instanceof HTMLInputElement) {
      this.omniboxInputEl = el;
    }
  }

  private maybeFocusOmnibar() {
    if (AppImpl.instance.omnibox.focusOmniboxNextRender) {
      const omniboxEl = this.omniboxInputEl;
      if (omniboxEl) {
        omniboxEl.focus();
        if (AppImpl.instance.omnibox.pendingCursorPlacement === undefined) {
          omniboxEl.select();
        } else {
          omniboxEl.setSelectionRange(
            AppImpl.instance.omnibox.pendingCursorPlacement,
            AppImpl.instance.omnibox.pendingCursorPlacement,
          );
        }
      }
      AppImpl.instance.omnibox.clearFocusFlag();
    }
  }

  private async maybeShowJsonWarning() {
    // Show warning if the trace is in JSON format.
    const isJsonTrace = this.trace?.traceInfo.traceType === 'json';
    const SHOWN_JSON_WARNING_KEY = 'shownJsonWarning';

    if (
      !isJsonTrace ||
      window.localStorage.getItem(SHOWN_JSON_WARNING_KEY) === 'true' ||
      AppImpl.instance.embeddedMode
    ) {
      // When in embedded mode, the host app will control which trace format
      // it passes to Perfetto, so we don't need to show this warning.
      return;
    }

    // Save that the warning has been shown. Value is irrelevant since only
    // the presence of key is going to be checked.
    window.localStorage.setItem(SHOWN_JSON_WARNING_KEY, 'true');

    showModal({
      title: 'Warning',
      content: m(
        'div',
        m(
          'span',
          'Perfetto UI features are limited for JSON traces. ',
          'We recommend recording ',
          m(
            'a',
            {href: 'https://perfetto.dev/docs/quickstart/chrome-tracing'},
            'proto-format traces',
          ),
          ' from Chrome.',
        ),
        m('br'),
      ),
    });
  }
}
