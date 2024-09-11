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
import {Actions} from '../common/actions';
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
import {maybeRenderFullscreenModalDialog} from '../widgets/modal';
import {onClickCopy} from './clipboard';
import {CookieConsent} from './cookie_consent';
import {getTimeSpanOfSelectionOrVisibleWindow, globals} from './globals';
import {toggleHelp} from './help_modal';
import {Notes} from './notes';
import {Omnibox, OmniboxOption} from './omnibox';
import {addQueryResultsTab} from './query_result_tab';
import {Sidebar} from './sidebar';
import {Topbar} from './topbar';
import {shareTrace} from './trace_attrs';
import {AggregationsTabs} from './aggregation_tab';
import {focusOtherFlow, moveByFocusedFlow} from './keyboard_event_handler';
import {publishPermalinkHash} from './publish';
import {OmniboxMode} from '../core/omnibox_manager';
import {PromptOption} from '../public/omnibox';
import {DisposableStack} from '../base/disposable_stack';
import {Spinner} from '../widgets/spinner';

function renderPermalink(): m.Children {
  const hash = globals.permalinkHash;
  if (!hash) return null;
  const url = `${self.location.origin}/#!/?s=${hash}`;
  const linkProps = {title: 'Click to copy the URL', onclick: onClickCopy(url)};

  return m('.alert-permalink', [
    m('div', 'Permalink: ', m(`a[href=${url}]`, linkProps, url)),
    m(
      'button',
      {
        onclick: () => publishPermalinkHash(undefined),
      },
      m('i.material-icons.disallow-selection', 'close'),
    ),
  ]);
}

class Alerts implements m.ClassComponent {
  view() {
    return m('.alerts', renderPermalink());
  }
}

export class UiMain implements m.ClassComponent {
  private trash = new DisposableStack();
  static readonly OMNIBOX_INPUT_REF = 'omnibox';
  private omniboxInputEl?: HTMLInputElement;
  private recentCommands: string[] = [];

  constructor() {
    this.trash.use(new Notes());
    this.trash.use(new AggregationsTabs());
  }

  private cmds: Command[] = [
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
            displayName: 'Trace nanoseconds (with locale-specific formatting)',
          },
        ];
        const promptText = 'Select format...';

        const result = await globals.omnibox.prompt(promptText, options);
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

        const result = await globals.omnibox.prompt(promptText, options);
        if (result === undefined) return;
        setDurationPrecision(result as DurationPrecision);
        raf.scheduleFullRedraw();
      },
    },
    {
      id: 'perfetto.TogglePerformanceMetrics',
      name: 'Toggle performance metrics',
      callback: () => {
        globals.dispatch(Actions.togglePerfDebug({}));
      },
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
        globals.searchManager.stepForward();
      },
      defaultHotkey: 'Enter',
    },
    {
      id: 'perfetto.SearchPrev',
      name: 'Go to previous search result',
      callback: () => {
        globals.searchManager.stepBackwards();
      },
      defaultHotkey: 'Shift+Enter',
    },
    {
      id: 'perfetto.OpenCommandPalette',
      name: 'Open command palette',
      callback: () => globals.omnibox.setMode(OmniboxMode.Command),
      defaultHotkey: '!Mod+Shift+P',
    },
    {
      id: 'perfetto.RunQuery',
      name: 'Run query',
      callback: () => globals.omnibox.setMode(OmniboxMode.Query),
    },
    {
      id: 'perfetto.Search',
      name: 'Search',
      callback: () => globals.omnibox.setMode(OmniboxMode.Search),
      defaultHotkey: '/',
    },
    {
      id: 'perfetto.ShowHelp',
      name: 'Show help',
      callback: () => toggleHelp(),
      defaultHotkey: '?',
    },
    {
      id: 'perfetto.CopyTimeWindow',
      name: `Copy selected time window to clipboard`,
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow();
        const query = `ts >= ${window.start} and ts < ${window.end}`;
        copyToClipboard(query);
      },
    },
    {
      id: 'perfetto.FocusSelection',
      name: 'Focus current selection',
      callback: () => globals.selectionManager.scrollToCurrentSelection(),
      defaultHotkey: 'F',
    },
    {
      id: 'perfetto.Deselect',
      name: 'Deselect',
      callback: () => {
        globals.selectionManager.clear();
      },
      defaultHotkey: 'Escape',
    },
    {
      id: 'perfetto.SetTemporarySpanNote',
      name: 'Set the temporary span note based on the current selection',
      callback: async () => {
        const range = await globals.selectionManager.findTimeRangeOfSelection();
        if (range) {
          globals.noteManager.addSpanNote({
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
      callback: async () => {
        const range = await globals.selectionManager.findTimeRangeOfSelection();
        if (range) {
          globals.noteManager.addSpanNote({start: range.start, end: range.end});
        }
      },
      defaultHotkey: 'Shift+M',
    },
    {
      id: 'perfetto.RemoveSelectedNote',
      name: 'Remove selected note',
      callback: () => {
        const selection = globals.selectionManager.selection;
        if (selection.kind === 'note') {
          globals.noteManager.removeNote(selection.id);
        }
      },
      defaultHotkey: 'Delete',
    },
    {
      id: 'perfetto.NextFlow',
      name: 'Next flow',
      callback: () => focusOtherFlow('Forward'),
      defaultHotkey: 'Mod+]',
    },
    {
      id: 'perfetto.PrevFlow',
      name: 'Prev flow',
      callback: () => focusOtherFlow('Backward'),
      defaultHotkey: 'Mod+[',
    },
    {
      id: 'perfetto.MoveNextFlow',
      name: 'Move next flow',
      callback: () => moveByFocusedFlow('Forward'),
      defaultHotkey: ']',
    },
    {
      id: 'perfetto.MovePrevFlow',
      name: 'Move prev flow',
      callback: () => moveByFocusedFlow('Backward'),
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
        let tracksToSelect: string[] = [];
        const selection = globals.selectionManager.selection;
        if (selection.kind === 'area') {
          // Something is already selected, let's see if it covers the entire
          // span of the trace or not
          const coversEntireTimeRange =
            globals.traceContext.start === selection.start &&
            globals.traceContext.end === selection.end;
          if (!coversEntireTimeRange) {
            // If the current selection is an area which does not cover the
            // entire time range, preserve the list of selected tracks and
            // expand the time range.
            tracksToSelect = selection.trackUris;
          } else {
            // If the entire time range is already covered, update the selection
            // to cover all tracks.
            tracksToSelect = globals.workspace.flatTracks.map((t) => t.uri);
          }
        } else {
          // If the current selection is not an area, select all.
          tracksToSelect = globals.workspace.flatTracks.map((t) => t.uri);
        }
        const {start, end} = globals.traceContext;
        globals.selectionManager.setArea({
          start,
          end,
          trackUris: tracksToSelect,
        });
      },
      defaultHotkey: 'Mod+A',
    },
  ];

  commands() {
    return this.cmds;
  }

  private renderOmnibox(): m.Children {
    const msgTTL = globals.state.status.timestamp + 1 - Date.now() / 1e3;
    const engineIsBusy =
      globals.state.engine !== undefined && !globals.state.engine.ready;

    if (msgTTL > 0 || engineIsBusy) {
      setTimeout(() => raf.scheduleFullRedraw(), msgTTL * 1000);
      return m(
        `.omnibox.message-mode`,
        m(`input[readonly][disabled][ref=omnibox]`, {
          value: '',
          placeholder: globals.state.status.msg,
        }),
      );
    }

    const omniboxMode = globals.omnibox.mode;

    if (omniboxMode === OmniboxMode.Command) {
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
    const prompt = assertExists(globals.omnibox.pendingPrompt);

    let options: OmniboxOption[] | undefined = undefined;

    if (prompt.options) {
      const fuzzy = new FuzzyFinder(
        prompt.options,
        ({displayName}) => displayName,
      );
      const result = fuzzy.find(globals.omnibox.text);
      options = result.map((result) => {
        return {
          key: result.item.key,
          displayName: result.segments,
        };
      });
    }

    return m(Omnibox, {
      value: globals.omnibox.text,
      placeholder: prompt.text,
      inputRef: UiMain.OMNIBOX_INPUT_REF,
      extraClasses: 'prompt-mode',
      closeOnOutsideClick: true,
      options,
      selectedOptionIndex: globals.omnibox.selectionIndex,
      onSelectedOptionChanged: (index) => {
        globals.omnibox.setSelectionIndex(index);
        raf.scheduleFullRedraw();
      },
      onInput: (value) => {
        globals.omnibox.setText(value);
        globals.omnibox.setSelectionIndex(0);
        raf.scheduleFullRedraw();
      },
      onSubmit: (value, _alt) => {
        globals.omnibox.resolvePrompt(value);
      },
      onClose: () => {
        globals.omnibox.rejectPrompt();
      },
    });
  }

  renderCommandOmnibox(): m.Children {
    const cmdMgr = globals.commandManager;

    // Fuzzy-filter commands by the filter string.
    const filteredCmds = cmdMgr.fuzzyFilterCommands(globals.omnibox.text);

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
      value: globals.omnibox.text,
      placeholder: 'Filter commands...',
      inputRef: UiMain.OMNIBOX_INPUT_REF,
      extraClasses: 'command-mode',
      options,
      closeOnSubmit: true,
      closeOnOutsideClick: true,
      selectedOptionIndex: globals.omnibox.selectionIndex,
      onSelectedOptionChanged: (index) => {
        globals.omnibox.setSelectionIndex(index);
        raf.scheduleFullRedraw();
      },
      onInput: (value) => {
        globals.omnibox.setText(value);
        globals.omnibox.setSelectionIndex(0);
        raf.scheduleFullRedraw();
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        globals.omnibox.reset();
      },
      onSubmit: (key: string) => {
        this.addRecentCommand(key);
        cmdMgr.runCommand(key);
      },
      onGoBack: () => {
        globals.omnibox.reset();
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
      value: globals.omnibox.text,
      placeholder: ph,
      inputRef: UiMain.OMNIBOX_INPUT_REF,
      extraClasses: 'query-mode',

      onInput: (value) => {
        globals.omnibox.setText(value);
        raf.scheduleFullRedraw();
      },
      onSubmit: (query, alt) => {
        const config = {
          query: undoCommonChatAppReplacements(query),
          title: alt ? 'Pinned query' : 'Omnibox query',
        };
        const tag = alt ? undefined : 'omnibox_query';
        addQueryResultsTab(config, tag);
      },
      onClose: () => {
        globals.omnibox.setText('');
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        globals.omnibox.reset();
        raf.scheduleFullRedraw();
      },
      onGoBack: () => {
        globals.omnibox.reset();
      },
    });
  }

  renderSearchOmnibox(): m.Children {
    return m(Omnibox, {
      value: globals.omnibox.text,
      placeholder: "Search or type '>' for commands or ':' for SQL mode",
      inputRef: UiMain.OMNIBOX_INPUT_REF,
      onInput: (value, _prev) => {
        if (value === '>') {
          globals.omnibox.setMode(OmniboxMode.Command);
          return;
        } else if (value === ':') {
          globals.omnibox.setMode(OmniboxMode.Query);
          return;
        }
        globals.omnibox.setText(value);
        if (value.length >= 4) {
          globals.searchManager.search(value);
        } else {
          globals.searchManager.reset();
        }
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (value, _mod, shift) => {
        globals.searchManager.search(value);
        if (shift) {
          globals.searchManager.stepBackwards();
        } else {
          globals.searchManager.stepForward();
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
    const results = globals.searchManager.searchResults;
    if (globals.searchManager.searchInProgress) {
      children.push(m('.current', m(Spinner)));
    } else if (results !== undefined) {
      const index = globals.searchManager.resultIndex;
      const total = results.totalResults ?? 0;
      children.push(
        m('.current', `${total === 0 ? '0 / 0' : `${index + 1} / ${total}`}`),
        m(
          'button',
          {
            onclick: () => {
              globals.searchManager.stepBackwards();
            },
          },
          m('i.material-icons.left', 'keyboard_arrow_left'),
        ),
        m(
          'button',
          {
            onclick: () => {
              globals.searchManager.stepForward();
            },
          },
          m('i.material-icons.right', 'keyboard_arrow_right'),
        ),
      );
    }
    return m('.stepthrough', children);
  }

  view({children}: m.Vnode): m.Children {
    const hotkeys: HotkeyConfig[] = [];
    const commands = globals.commandManager.commands;
    for (const {id, defaultHotkey} of commands) {
      if (defaultHotkey) {
        hotkeys.push({
          callback: () => {
            globals.commandManager.runCommand(id);
          },
          hotkey: defaultHotkey,
        });
      }
    }

    return m(
      HotkeyContext,
      {hotkeys},
      m(
        'main',
        m(Sidebar),
        m(Topbar, {
          omnibox: this.renderOmnibox(),
        }),
        m(Alerts),
        children,
        m(CookieConsent),
        maybeRenderFullscreenModalDialog(),
        globals.state.perfDebug && m('.perf-stats'),
      ),
    );
  }

  oncreate({dom}: m.VnodeDOM) {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();

    // Register each command with the command manager
    this.cmds.forEach((cmd) => {
      const dispose = globals.commandManager.registerCommand(cmd);
      this.trash.use(dispose);
    });
  }

  onupdate({dom}: m.VnodeDOM) {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
  }

  onremove(_: m.VnodeDOM) {
    this.trash.dispose();
    this.omniboxInputEl = undefined;
  }

  private updateOmniboxInputRef(dom: Element): void {
    const el = findRef(dom, UiMain.OMNIBOX_INPUT_REF);
    if (el && el instanceof HTMLInputElement) {
      this.omniboxInputEl = el;
    }
  }

  private maybeFocusOmnibar() {
    if (globals.omnibox.focusOmniboxNextRender) {
      const omniboxEl = this.omniboxInputEl;
      if (omniboxEl) {
        omniboxEl.focus();
        if (globals.omnibox.pendingCursorPlacement === undefined) {
          omniboxEl.select();
        } else {
          omniboxEl.setSelectionRange(
            globals.omnibox.pendingCursorPlacement,
            globals.omnibox.pendingCursorPlacement,
          );
        }
      }
      globals.omnibox.clearFocusFlag();
    }
  }
}
