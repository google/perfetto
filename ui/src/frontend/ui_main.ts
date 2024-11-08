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
import {
  DurationPrecision,
  setDurationPrecision,
  setTimestampFormat,
  TimestampFormat,
} from '../core/timestamp_format';
import {raf} from '../core/raf_scheduler';
import {Command} from '../public/command';
import {HotkeyConfig, HotkeyContext} from '../widgets/hotkey_context';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {maybeRenderFullscreenModalDialog, showModal} from '../widgets/modal';
import {CookieConsent} from './cookie_consent';
import {toggleHelp} from './help_modal';
import {Omnibox, OmniboxOption} from './omnibox';
import {addQueryResultsTab} from '../public/lib/query_table/query_result_tab';
import {Sidebar} from './sidebar';
import {Topbar} from './topbar';
import {shareTrace} from './trace_share_utils';
import {AggregationsTabs} from './aggregation_tab';
import {OmniboxMode} from '../core/omnibox_manager';
import {PromptOption} from '../public/omnibox';
import {DisposableStack} from '../base/disposable_stack';
import {Spinner} from '../widgets/spinner';
import {TraceImpl} from '../core/trace_impl';
import {AppImpl} from '../core/app_impl';
import {NotesEditorTab} from './notes_panel';
import {NotesListEditor} from './notes_list_editor';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../public/utils';

const OMNIBOX_INPUT_REF = 'omnibox';

// This wrapper creates a new instance of UiMainPerTrace for each new trace
// loaded (including the case of no trace at the beginning).
export class UiMain implements m.ClassComponent {
  view({children}: m.CVnode) {
    const currentTraceId = AppImpl.instance.trace?.engine.engineId ?? '';
    return [m(UiMainPerTrace, {key: currentTraceId}, children)];
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

    // Register the aggregation tabs.
    this.trash.use(new AggregationsTabs(trace));

    // Register the notes manager+editor.
    this.trash.use(trace.tabs.registerDetailsPanel(new NotesEditorTab(trace)));

    this.trash.use(
      trace.tabs.registerTab({
        uri: 'notes.manager',
        isEphemeral: false,
        content: {
          getTitle: () => 'Notes & markers',
          render: () => m(NotesListEditor, {trace}),
        },
      }),
    );

    const cmds: Command[] = [
      {
        id: 'perfetto.SetTimestampFormat',
        name: 'Set timestamp and duration format',
        callback: async () => {
          const options: PromptOption[] = [
            {key: TimestampFormat.Timecode, displayName: 'Timecode'},
            {key: TimestampFormat.UTC, displayName: 'Realtime (UTC)'},
            {
              key: TimestampFormat.TraceTz,
              displayName: 'Realtime (Trace TZ)',
            },
            {key: TimestampFormat.Seconds, displayName: 'Seconds'},
            {key: TimestampFormat.Milliseoncds, displayName: 'Milliseconds'},
            {key: TimestampFormat.Microseconds, displayName: 'Microseconds'},
            {key: TimestampFormat.TraceNs, displayName: 'Trace nanoseconds'},
            {
              key: TimestampFormat.TraceNsLocale,
              displayName:
                'Trace nanoseconds (with locale-specific formatting)',
            },
          ];
          const promptText = 'Select format...';

          const result = await app.omnibox.prompt(promptText, options);
          if (result === undefined) return;
          setTimestampFormat(result as TimestampFormat);
          raf.scheduleFullRedraw();
        },
      },
      {
        id: 'perfetto.SetDurationPrecision',
        name: 'Set duration precision',
        callback: async () => {
          const options: PromptOption[] = [
            {key: DurationPrecision.Full, displayName: 'Full'},
            {
              key: DurationPrecision.HumanReadable,
              displayName: 'Human readable',
            },
          ];
          const promptText = 'Select duration precision mode...';

          const result = await app.omnibox.prompt(promptText, options);
          if (result === undefined) return;
          setDurationPrecision(result as DurationPrecision);
          raf.scheduleFullRedraw();
        },
      },
      {
        id: 'perfetto.TogglePerformanceMetrics',
        name: 'Toggle performance metrics',
        callback: () => app.setPerfDebuggingEnabled(!app.perfDebugging),
      },
      {
        id: 'perfetto.ShareTrace',
        name: 'Share trace',
        callback: shareTrace,
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
        callback: () => trace.selection.scrollToCurrentSelection(),
        defaultHotkey: 'F',
      },
      {
        id: 'perfetto.Deselect',
        name: 'Deselect',
        callback: () => {
          trace.selection.clear();
        },
        defaultHotkey: 'Escape',
      },
      {
        id: 'perfetto.SetTemporarySpanNote',
        name: 'Set the temporary span note based on the current selection',
        callback: () => {
          const range = trace.selection.findTimeRangeOfSelection();
          if (range) {
            trace.notes.addSpanNote({
              start: range.start,
              end: range.end,
              id: '__temp__',
            });
          }
        },
        defaultHotkey: 'M',
      },
      {
        id: 'perfetto.AddSpanNote',
        name: 'Add a new span note based on the current selection',
        callback: () => {
          const range = trace.selection.findTimeRangeOfSelection();
          if (range) {
            trace.notes.addSpanNote({
              start: range.start,
              end: range.end,
            });
          }
        },
        defaultHotkey: 'Shift+M',
      },
      {
        id: 'perfetto.RemoveSelectedNote',
        name: 'Remove selected note',
        callback: () => {
          const selection = trace.selection.selection;
          if (selection.kind === 'note') {
            trace.notes.removeNote(selection.id);
          }
        },
        defaultHotkey: 'Delete',
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
        name: 'Convert the current selection to an area selection',
        callback: () => {
          const selection = trace.selection.selection;
          const range = trace.selection.findTimeRangeOfSelection();
          if (selection.kind === 'track_event' && range) {
            trace.selection.selectArea({
              start: range.start,
              end: range.end,
              trackUris: [selection.trackUri],
            });
          }
        },
        // TODO(stevegolton): Decide on a sensible hotkey.
        // defaultHotkey: 'L',
      },
      {
        id: 'perfetto.ToggleDrawer',
        name: 'Toggle drawer',
        defaultHotkey: 'Q',
        callback: () => trace.tabs.toggleTabPanelVisibility(),
      },
    ];

    // Register each command with the command manager
    cmds.forEach((cmd) => {
      this.trash.use(trace.commands.registerCommand(cmd));
    });
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
        raf.scheduleFullRedraw();
      },
      onInput: (value) => {
        omnibox.setText(value);
        omnibox.setSelectionIndex(0);
        raf.scheduleFullRedraw();
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

    // Sort by recentsIndex then by alphabetical order
    const sorted = commandsWithHeuristics.sort((a, b) => {
      if (b.recentsIndex === a.recentsIndex) {
        return a.cmd.name.localeCompare(b.cmd.name);
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
        raf.scheduleFullRedraw();
      },
      onInput: (value) => {
        omnibox.setText(value);
        omnibox.setSelectionIndex(0);
        raf.scheduleFullRedraw();
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
        raf.scheduleFullRedraw();
      },
      onSubmit: (query, alt) => {
        const config = {
          query: undoCommonChatAppReplacements(query),
          title: alt ? 'Pinned query' : 'Omnibox query',
        };
        const tag = alt ? undefined : 'omnibox_query';
        const trace = AppImpl.instance.trace;
        if (trace === undefined) return; // No trace loaded
        addQueryResultsTab(trace, config, tag);
      },
      onClose: () => {
        AppImpl.instance.omnibox.setText('');
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        AppImpl.instance.omnibox.reset();
        raf.scheduleFullRedraw();
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

  view({children}: m.Vnode): m.Children {
    const hotkeys: HotkeyConfig[] = [];
    for (const {id, defaultHotkey} of AppImpl.instance.commands.commands) {
      if (defaultHotkey) {
        hotkeys.push({
          callback: () => AppImpl.instance.commands.runCommand(id),
          hotkey: defaultHotkey,
        });
      }
    }

    return m(
      HotkeyContext,
      {hotkeys},
      m(
        'main',
        m(Sidebar, {trace: this.trace}),
        m(Topbar, {
          omnibox: this.renderOmnibox(),
          trace: this.trace,
        }),
        children,
        m(CookieConsent),
        maybeRenderFullscreenModalDialog(),
        AppImpl.instance.perfDebugging && m('.perf-stats'),
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
      buttons: [],
    });
  }
}
