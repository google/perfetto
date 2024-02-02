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
import {Trash} from '../base/disposable';
import {findRef} from '../base/dom_utils';
import {FuzzyFinder} from '../base/fuzzy';
import {assertExists} from '../base/logging';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {
  duration,
  Span,
  Time,
  time,
  TimeSpan,
} from '../base/time';
import {Actions} from '../common/actions';
import {runQuery} from '../common/queries';
import {
  DurationPrecision,
  setDurationPrecision,
  setTimestampFormat,
  TimestampFormat,
} from '../common/timestamp_format';
import {raf} from '../core/raf_scheduler';
import {Command} from '../public';
import {EngineProxy} from '../trace_processor/engine';
import {THREAD_STATE_TRACK_KIND} from '../tracks/thread_state';
import {HotkeyConfig, HotkeyContext} from '../widgets/hotkey_context';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {maybeRenderFullscreenModalDialog} from '../widgets/modal';

import {onClickCopy} from './clipboard';
import {CookieConsent} from './cookie_consent';
import {globals} from './globals';
import {toggleHelp} from './help_modal';
import {Notes} from './notes';
import {Omnibox, OmniboxOption} from './omnibox';
import {addQueryResultsTab} from './query_result_tab';
import {verticalScrollToTrack} from './scroll_helper';
import {executeSearch} from './search_handler';
import {Sidebar} from './sidebar';
import {Utid} from './sql_types';
import {getThreadInfo} from './thread_and_process_info';
import {Topbar} from './topbar';
import {shareTrace} from './trace_attrs';
import {addDebugSliceTrack} from './debug_tracks';
import {AggregationsTabs} from './aggregation_tab';
import {addSqlTableTab} from './sql_table/tab';
import {SqlTables} from './sql_table/well_known_tables';

function renderPermalink(): m.Children {
  const permalink = globals.state.permalink;
  if (!permalink.requestId || !permalink.hash) return null;
  const url = `${self.location.origin}/#!/?s=${permalink.hash}`;
  const linkProps = {title: 'Click to copy the URL', onclick: onClickCopy(url)};

  return m('.alert-permalink', [
    m('div', 'Permalink: ', m(`a[href=${url}]`, linkProps, url)),
    m('button',
      {
        onclick: () => globals.dispatch(Actions.clearPermalink({})),
      },
      m('i.material-icons.disallow-selection', 'close')),
  ]);
}

class Alerts implements m.ClassComponent {
  view() {
    return m('.alerts', renderPermalink());
  }
}

interface PromptOption {
  key: string;
  displayName: string;
}

interface Prompt {
  text: string;
  options?: PromptOption[];
  resolve(result: string): void;
  reject(): void;
}

enum OmniboxMode {
  Search,
  Query,
  Command,
  Prompt,
}

const criticalPathSliceColumns = {
  ts: 'ts',
  dur: 'dur',
  name: 'name',
};
const criticalPathsliceColumnNames = [
  'id',
  'utid',
  'ts',
  'dur',
  'name',
  'table_name',
];

const criticalPathsliceLiteColumns = {
  ts: 'ts',
  dur: 'dur',
  name: 'thread_name',
};
const criticalPathsliceLiteColumnNames = [
  'id',
  'utid',
  'ts',
  'dur',
  'thread_name',
  'process_name',
  'table_name',
];


export class App implements m.ClassComponent {
  private trash = new Trash();

  private omniboxMode: OmniboxMode = OmniboxMode.Search;
  private omniboxText = '';
  private queryText = '';
  private omniboxSelectionIndex = 0;
  private focusOmniboxNextRender = false;
  private pendingCursorPlacement = -1;
  private pendingPrompt?: Prompt;
  static readonly OMNIBOX_INPUT_REF = 'omnibox';
  private omniboxInputEl?: HTMLInputElement;
  private recentCommands: string[] = [];

  constructor() {
    this.trash.add(new Notes());
    this.trash.add(new AggregationsTabs());
  }

  private getEngine(): EngineProxy|undefined {
    const engineId = globals.getCurrentEngine()?.id;
    if (engineId === undefined) {
      return undefined;
    }
    const engine = globals.engines.get(engineId)?.getProxy('QueryPage');
    return engine;
  }

  private enterCommandMode(): void {
    this.omniboxMode = OmniboxMode.Command;
    this.resetOmnibox();
    this.rejectPendingPrompt();
    this.focusOmniboxNextRender = true;

    raf.scheduleFullRedraw();
  }

  private enterQueryMode(): void {
    this.omniboxMode = OmniboxMode.Query;
    this.resetOmnibox();
    this.rejectPendingPrompt();
    this.focusOmniboxNextRender = true;

    raf.scheduleFullRedraw();
  }

  private enterSearchMode(focusOmnibox: boolean): void {
    this.omniboxMode = OmniboxMode.Search;
    this.resetOmnibox();
    this.rejectPendingPrompt();

    if (focusOmnibox) {
      this.focusOmniboxNextRender = true;
    }

    globals.dispatch(Actions.setOmniboxMode({mode: 'SEARCH'}));

    raf.scheduleFullRedraw();
  }

  // Start a prompt. If options are supplied, the user must pick one from the
  // list, otherwise the input is free-form text.
  private prompt(text: string, options?: PromptOption[]): Promise<string> {
    this.omniboxMode = OmniboxMode.Prompt;
    this.resetOmnibox();
    this.rejectPendingPrompt();

    const promise = new Promise<string>((resolve, reject) => {
      this.pendingPrompt = {
        text,
        options,
        resolve,
        reject,
      };
    });

    this.focusOmniboxNextRender = true;
    raf.scheduleFullRedraw();

    return promise;
  }

  // Resolve the pending prompt with a value to return to the prompter.
  private resolvePrompt(value: string): void {
    if (this.pendingPrompt) {
      this.pendingPrompt.resolve(value);
      this.pendingPrompt = undefined;
    }
    this.enterSearchMode(false);
  }

  // Reject the prompt outright. Doing this will force the owner of the prompt
  // promise to catch, so only do this when things go seriously wrong.
  // Use |resolvePrompt(null)| to indicate cancellation.
  private rejectPrompt(): void {
    if (this.pendingPrompt) {
      this.pendingPrompt.reject();
      this.pendingPrompt = undefined;
    }
    this.enterSearchMode(false);
  }

  private getFirstUtidOfSelectionOrVisibleWindow(): number {
    const selection = globals.state.currentSelection;
    if (selection && selection.kind === 'AREA') {
      const selectedArea = globals.state.areas[selection.areaId];
      const firstThreadStateTrack = selectedArea.tracks.find((trackId) => {
        return globals.state.tracks[trackId];
      });

      if (firstThreadStateTrack) {
        const trackInfo = globals.state.tracks[firstThreadStateTrack];
        const trackDesc = globals.trackManager.resolveTrackInfo(trackInfo.uri);
        if (trackDesc?.kind === THREAD_STATE_TRACK_KIND &&
            trackDesc?.utid !== undefined) {
          return trackDesc?.utid;
        }
      }
    }

    return 0;
  }

  private cmds: Command[] = [
    {
      id: 'perfetto.SetTimestampFormat',
      name: 'Set timestamp and duration format',
      callback:
          async () => {
            const options: PromptOption[] = [
              {key: TimestampFormat.Timecode, displayName: 'Timecode'},
              {key: TimestampFormat.UTC, displayName: 'Realtime (UTC)'},
              {
                key: TimestampFormat.TraceTz,
                displayName: 'Realtime (Trace TZ)',
              },
              {key: TimestampFormat.Seconds, displayName: 'Seconds'},
              {key: TimestampFormat.Raw, displayName: 'Raw'},
              {
                key: TimestampFormat.RawLocale,
                displayName: 'Raw (with locale-specific formatting)',
              },
            ];
            const promptText = 'Select format...';

            try {
              const result = await this.prompt(promptText, options);
              setTimestampFormat(result as TimestampFormat);
              raf.scheduleFullRedraw();
            } catch {
              // Prompt was probably cancelled - do nothing.
            }
          },
    },
    {
      id: 'perfetto.SetDurationPrecision',
      name: 'Set duration precision',
      callback:
          async () => {
            const options: PromptOption[] = [
              {key: DurationPrecision.Full, displayName: 'Full'},
              {
                key: DurationPrecision.HumanReadable,
                displayName: 'Human readable',
              },
            ];
            const promptText = 'Select duration precision mode...';

            try {
              const result = await this.prompt(promptText, options);
              setDurationPrecision(result as DurationPrecision);
              raf.scheduleFullRedraw();
            } catch {
              // Prompt was probably cancelled - do nothing.
            }
          },
    },
    {
      id: 'perfetto.CriticalPathLite',
      name: `Critical path lite`,
      callback:
          async () => {
            const trackUtid = this.getFirstUtidOfSelectionOrVisibleWindow();
            const window = getTimeSpanOfSelectionOrVisibleWindow();
            const engine = this.getEngine();

            if (engine !== undefined && trackUtid != 0) {
              await runQuery(
                `SELECT IMPORT('sched.thread_executing_span');`,
                engine);
              await addDebugSliceTrack(
                engine,
                {
                  sqlSource: `
                   SELECT
                      cr.id,
                      cr.utid,
                      cr.ts,
                      cr.dur,
                      thread.name AS thread_name,
                      process.name AS process_name,
                      'thread_state' AS table_name
                    FROM
                      _thread_executing_span_critical_path(
                          ${trackUtid},
                          ${window.start},
                          ${window.end} - ${window.start}) cr
                    JOIN thread USING(utid)
                    JOIN process USING(upid)
                  `,
                  columns: criticalPathsliceLiteColumnNames,
                },
                (await getThreadInfo(engine, trackUtid as Utid)).name ??
                      '<thread name>',
                criticalPathsliceLiteColumns,
                criticalPathsliceLiteColumnNames);
            }
          },
    },
    {
      id: 'perfetto.CriticalPath',
      name: `Critical path`,
      callback:
          async () => {
            const trackUtid = this.getFirstUtidOfSelectionOrVisibleWindow();
            const window = getTimeSpanOfSelectionOrVisibleWindow();
            const engine = this.getEngine();

            if (engine !== undefined && trackUtid != 0) {
              await runQuery(
                `SELECT IMPORT('sched.thread_executing_span');`,
                engine);
              await addDebugSliceTrack(
                engine,
                {
                  sqlSource: `
                        SELECT cr.id, cr.utid, cr.ts, cr.dur, cr.name, cr.table_name
                        FROM
                        _critical_path_stack(
                          ${trackUtid},
                          ${window.start},
                          ${window.end} - ${
  window.start}, 1, 1, 1, 1) cr WHERE name IS NOT NULL
                  `,
                  columns: criticalPathsliceColumnNames,
                },
                (await getThreadInfo(engine, trackUtid as Utid)).name ??
                      '<thread name>',
                criticalPathSliceColumns,
                criticalPathsliceColumnNames);
            }
          },
    },
    {
      id: 'perfetto.CriticalPathPprof',
      name: `Critical path pprof`,
      callback:
          () => {
            const trackUtid = this.getFirstUtidOfSelectionOrVisibleWindow();
            const window = getTimeSpanOfSelectionOrVisibleWindow();
            const engine = this.getEngine();

            if (engine !== undefined && trackUtid != 0) {
              addQueryResultsTab({
                query: `SELECT IMPORT('sched.thread_executing_span');
                   SELECT *
                      FROM
                        _thread_executing_span_critical_path_graph(
                        "criical_path",
                         ${trackUtid},
                         ${window.start},
                         ${window.end} - ${window.start}) cr`,
                title: 'Critical path'});
            }
          },
    },
    {
      id: 'perfetto.ShowSliceTable',
      name: 'Show slice table',
      callback:
          () => {
            addSqlTableTab({
              table: SqlTables.slice,
              displayName: 'slice',
            });
          },
    },
    {
      id: 'perfetto.TogglePerformanceMetrics',
      name: 'Toggle performance metrics',
      callback:
          () => {
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
      callback:
          () => {
            executeSearch();
          },
      defaultHotkey: 'Enter',
    },
    {
      id: 'perfetto.SearchPrev',
      name: 'Go to previous search result',
      callback:
          () => {
            executeSearch(true);
          },
      defaultHotkey: 'Shift+Enter',
    },
    {
      id: 'perfetto.OpenCommandPalette',
      name: 'Open Command Palette',
      callback: () => this.enterCommandMode(),
      defaultHotkey: '!Mod+Shift+P',
    },
    {
      id: 'perfetto.RunQuery',
      name: 'Run Query',
      callback: () => this.enterQueryMode(),
      defaultHotkey: '!Mod+O',
    },
    {
      id: 'perfetto.Search',
      name: 'Search',
      callback: () => this.enterSearchMode(true),
      defaultHotkey: '!Mod+S',
    },
    {
      id: 'perfetto.ShowHelp',
      name: 'Show help',
      callback: () => toggleHelp(),
      defaultHotkey: '?',
    },
    {
      id: 'perfetto.RunQueryInSelectedTimeWindow',
      name: `Run query in selected time window`,
      callback:
          () => {
            const window = getTimeSpanOfSelectionOrVisibleWindow();
            this.enterQueryMode();
            this.queryText =
                `select  where ts >= ${window.start} and ts < ${window.end}`;
            this.pendingCursorPlacement = 7;
          },
    },
    {
      id: 'perfetto.CopyTimeWindow',
      name: `Copy selected time window to clipboard`,
      callback:
          () => {
            const window = getTimeSpanOfSelectionOrVisibleWindow();
            const query = `ts >= ${window.start} and ts < ${window.end}`;
            copyToClipboard(query);
          },
    },
    {
      // Selects & reveals the first track on the timeline with a given URI.
      id: 'perfetto.FindTrack',
      name: 'Find track by URI',
      callback:
          async () => {
            const tracks = globals.trackManager.getAllTracks();
            const options = tracks.map(({uri}): PromptOption => {
              return {key: uri, displayName: uri};
            });

            // Sort tracks in a natural sort order
            const collator = new Intl.Collator('en', {
              numeric: true,
              sensitivity: 'base',
            });
            const sortedOptions = options.sort((a, b) => {
              return collator.compare(a.displayName, b.displayName);
            });

            try {
              const selectedUri =
                  await this.prompt('Choose a track...', sortedOptions);

              // Find the first track with this URI
              const firstTrack = Object.values(globals.state.tracks)
                .find(({uri}) => uri === selectedUri);
              if (firstTrack) {
                console.log(firstTrack);
                verticalScrollToTrack(firstTrack.key, true);
                const traceTime = globals.stateTraceTimeTP();
                globals.makeSelection(
                  Actions.selectArea({
                    area: {
                      start: traceTime.start,
                      end: traceTime.end,
                      tracks: [firstTrack.key],
                    },
                  }),
                );
              } else {
                alert(`No tracks with uri ${selectedUri} on the timeline`);
              }
            } catch {
              // Prompt was probably cancelled - do nothing.
            }
          },
    },
  ];

  commands() {
    return this.cmds;
  }

  private rejectPendingPrompt() {
    if (this.pendingPrompt) {
      this.pendingPrompt.reject();
      this.pendingPrompt = undefined;
    }
  }

  private resetOmnibox() {
    this.omniboxText = '';
    this.omniboxSelectionIndex = 0;
  }

  private renderOmnibox(): m.Children {
    const msgTTL = globals.state.status.timestamp + 1 - Date.now() / 1e3;
    const engineIsBusy =
        globals.state.engine !== undefined && !globals.state.engine.ready;

    if (msgTTL > 0 || engineIsBusy) {
      setTimeout(() => raf.scheduleFullRedraw(), msgTTL * 1000);
      return m(
        `.omnibox.message-mode`, m(`input[readonly][disabled][ref=omnibox]`, {
          value: '',
          placeholder: globals.state.status.msg,
        }));
    }

    if (this.omniboxMode === OmniboxMode.Command) {
      return this.renderCommandOmnibox();
    } else if (this.omniboxMode === OmniboxMode.Prompt) {
      return this.renderPromptOmnibox();
    } else if (this.omniboxMode === OmniboxMode.Query) {
      return this.renderQueryOmnibox();
    } else if (this.omniboxMode === OmniboxMode.Search) {
      return this.renderSearchOmnibox();
    } else {
      const x: never = this.omniboxMode;
      throw new Error(`Unhandled omnibox mode ${x}`);
    }
  }

  renderPromptOmnibox(): m.Children {
    const prompt = assertExists(this.pendingPrompt);

    let options: OmniboxOption[]|undefined = undefined;

    if (prompt.options) {
      const fuzzy =
          new FuzzyFinder(prompt.options, ({displayName}) => displayName);
      const result = fuzzy.find(this.omniboxText);
      options = result.map((result) => {
        return {
          key: result.item.key,
          displayName: result.segments,
        };
      });
    }

    return m(Omnibox, {
      value: this.omniboxText,
      placeholder: prompt.text,
      inputRef: App.OMNIBOX_INPUT_REF,
      extraClasses: 'prompt-mode',
      closeOnOutsideClick: true,
      options,
      selectedOptionIndex: this.omniboxSelectionIndex,
      onSelectedOptionChanged: (index) => {
        this.omniboxSelectionIndex = index;
        raf.scheduleFullRedraw();
      },
      onInput: (value) => {
        this.omniboxText = value;
        this.omniboxSelectionIndex = 0;
        raf.scheduleFullRedraw();
      },
      onSubmit: (value, _alt) => {
        this.resolvePrompt(value);
      },
      onClose: () => {
        this.rejectPrompt();
      },
    });
  }

  renderCommandOmnibox(): m.Children {
    const cmdMgr = globals.commandManager;

    // Fuzzy-filter commands by the filter string.
    const filteredCmds = cmdMgr.fuzzyFilterCommands(this.omniboxText);

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
      value: this.omniboxText,
      placeholder: 'Filter commands...',
      inputRef: App.OMNIBOX_INPUT_REF,
      extraClasses: 'command-mode',
      options,
      closeOnSubmit: true,
      closeOnOutsideClick: true,
      selectedOptionIndex: this.omniboxSelectionIndex,
      onSelectedOptionChanged: (index) => {
        this.omniboxSelectionIndex = index;
        raf.scheduleFullRedraw();
      },
      onInput: (value) => {
        this.omniboxText = value;
        this.omniboxSelectionIndex = 0;
        raf.scheduleFullRedraw();
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        this.enterSearchMode(false);
        raf.scheduleFullRedraw();
      },
      onSubmit: (key: string) => {
        this.addRecentCommand(key);
        cmdMgr.runCommand(key);
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
      value: this.queryText,
      placeholder: ph,
      inputRef: App.OMNIBOX_INPUT_REF,
      extraClasses: 'query-mode',
      onInput: (value) => {
        this.queryText = value;
        raf.scheduleFullRedraw();
      },
      onSubmit: (query, alt) => {
        const config = {
          query: undoCommonChatAppReplacements(query),
          title: alt ? 'Pinned query' : 'Omnibox query',
        };
        const tag = alt? undefined : 'omnibox_query';
        addQueryResultsTab(config, tag);
      },
      onClose: () => {
        this.queryText = '';
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
        this.enterSearchMode(false);
        raf.scheduleFullRedraw();
      },
    });
  }

  renderSearchOmnibox(): m.Children {
    const omniboxState = globals.state.omniboxState;
    const displayStepThrough =
        omniboxState.omnibox.length >= 4 || omniboxState.force;

    return m(Omnibox, {
      value: globals.state.omniboxState.omnibox,
      placeholder: 'Search or type \'>\' for commands or \':\' for SQL mode',
      inputRef: App.OMNIBOX_INPUT_REF,
      onInput: (value, prev) => {
        if (prev === '') {
          if (value === '>') {
            this.enterCommandMode();
            return;
          } else if (value === ':') {
            this.enterQueryMode();
            return;
          }
        }
        globals.dispatch(Actions.setOmnibox({omnibox: value, mode: 'SEARCH'}));
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (value, _mod, shift) => {
        executeSearch(shift);
        globals.dispatch(
          Actions.setOmnibox({omnibox: value, mode: 'SEARCH', force: true}));
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      rightContent: displayStepThrough && this.renderStepThrough(),
    });
  }

  private renderStepThrough() {
    return m(
      '.stepthrough',
      m('.current',
        `${
          globals.currentSearchResults.totalResults === 0 ?
            '0 / 0' :
            `${globals.state.searchIndex + 1} / ${
              globals.currentSearchResults.totalResults}`}`),
      m('button',
        {
          onclick: () => {
            executeSearch(true /* reverse direction */);
          },
        },
        m('i.material-icons.left', 'keyboard_arrow_left')),
      m('button',
        {
          onclick: () => {
            executeSearch();
          },
        },
        m('i.material-icons.right', 'keyboard_arrow_right')));
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
      const dispose = globals.commandManager.registry.register(cmd);
      this.trash.add(dispose);
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
    const el = findRef(dom, App.OMNIBOX_INPUT_REF);
    if (el && el instanceof HTMLInputElement) {
      this.omniboxInputEl = el;
    }
  }

  private maybeFocusOmnibar() {
    if (this.focusOmniboxNextRender) {
      const omniboxEl = this.omniboxInputEl;
      if (omniboxEl) {
        omniboxEl.focus();
        if (this.pendingCursorPlacement === -1) {
          omniboxEl.select();
        } else {
          omniboxEl.setSelectionRange(
            this.pendingCursorPlacement, this.pendingCursorPlacement);
          this.pendingCursorPlacement = -1;
        }
      }
      this.focusOmniboxNextRender = false;
    }
  }
}

// Returns the time span of the current selection, or the visible window if
// there is no current selection.
function getTimeSpanOfSelectionOrVisibleWindow(): Span<time, duration> {
  const range = globals.findTimeRangeOfSelection();
  if (range.end !== Time.INVALID && range.start !== Time.INVALID) {
    return new TimeSpan(range.start, range.end);
  } else {
    return globals.stateVisibleTime();
  }
}
