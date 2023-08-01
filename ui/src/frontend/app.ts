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

import {Trash} from '../base/disposable';
import {findRef} from '../base/dom_utils';
import {FuzzyFinder} from '../base/fuzzy';
import {assertExists} from '../base/logging';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {Actions} from '../common/actions';
import {setTimestampFormat, TimestampFormat} from '../common/time';
import {raf} from '../core/raf_scheduler';

import {addTab} from './bottom_tab';
import {onClickCopy} from './clipboard';
import {CookieConsent} from './cookie_consent';
import {globals} from './globals';
import {fullscreenModalContainer} from './modal';
import {Omnibox, OmniboxOption} from './omnibox';
import {runQueryInNewTab} from './query_result_tab';
import {executeSearch} from './search_handler';
import {Sidebar} from './sidebar';
import {SqlTableTab} from './sql_table/tab';
import {SqlTables} from './sql_table/well_known_tables';
import {Topbar} from './topbar';
import {shareTrace} from './trace_attrs';
import {HotkeyConfig, HotkeyContext} from './widgets/hotkey_context';

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

export class App implements m.ClassComponent {
  private trash = new Trash();

  private omniboxMode: OmniboxMode = OmniboxMode.Search;
  private omniboxText = '';
  private queryText = '';
  private omniboxSelectionIndex = 0;
  private focusOmniboxNextRender = false;
  private pendingPrompt?: Prompt;
  static readonly OMNIBOX_INPUT_REF = 'omnibox';
  private omniboxInputEl?: HTMLInputElement;

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

  private hotkeys: HotkeyConfig[] = [
    {
      key: 'p',
      mods: ['Mod', 'Shift'],
      allowInEditable: true,
      callback: () => this.enterCommandMode(),
    },
    {
      key: 'o',
      mods: ['Mod'],
      allowInEditable: true,
      callback: () => this.enterQueryMode(),
    },
    {
      key: 's',
      mods: ['Mod'],
      allowInEditable: true,
      callback: () => this.enterSearchMode(true),
    },
    {
      key: 'b',
      mods: ['Mod'],
      allowInEditable: true,
      callback:
          () => {
            globals.commandManager.runCommand(
                'dev.perfetto.CoreCommands.ToggleLeftSidebar');
          },
    },
  ];

  private cmds = [
    {
      id: 'perfetto.SetTimestampFormat',
      name: 'Set timestamp format',
      callback:
          async () => {
            const options: PromptOption[] = [
              {key: TimestampFormat.Timecode, displayName: 'Timecode'},
              {key: TimestampFormat.Seconds, displayName: 'Seconds'},
              {key: TimestampFormat.Raw, displayName: 'Raw'},
              {
                key: TimestampFormat.RawLocale,
                displayName: 'Raw (with locale-specific formatting)',
              },
            ];
            const promptText = 'Select timecode format...';

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
      id: 'perfetto.ShowSliceTable',
      name: 'Show slice table',
      callback:
          () => {
            addTab({
              kind: SqlTableTab.kind,
              config: {
                table: SqlTables.slice,
                displayName: 'slice',
              },
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
    },
    {
      id: 'perfetto.SearchPrev',
      name: 'Go to previous search result',
      callback:
          () => {
            executeSearch(true);
          },
    },
    {
      id: 'perfetto.OpenCommandPalette',
      name: 'Open Command Palette',
      callback: () => this.enterCommandMode(),
    },
    {
      id: 'perfetto.RunQuery',
      name: 'Run Query',
      callback: () => this.enterQueryMode(),
    },
    {
      id: 'perfetto.Search',
      name: 'Search',
      callback: () => this.enterSearchMode(true),
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
          `.omnibox.message-mode`,
          m(`input[placeholder=${
                globals.state.status.msg}][readonly][disabled][ref=omnibox]`,
            {
              value: '',
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
    const filteredCmds = cmdMgr.fuzzyFilterCommands(this.omniboxText);
    const options: OmniboxOption[] = filteredCmds.map(({segments, id}) => {
      return {
        key: id,
        displayName: segments,
      };
    });

    return m(Omnibox, {
      value: this.omniboxText,
      placeholder: 'Start typing a command...',
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
        cmdMgr.runCommand(key);
      },
    });
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
      onSubmit: (value, alt) => {
        runQueryInNewTab(
            undoCommonChatAppReplacements(value),
            alt ? 'Pinned query' : 'Omnibox query',
            alt ? undefined : 'omnibox_query');
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
      placeholder: 'Search...',
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
    return m(
        HotkeyContext,
        {hotkeys: this.hotkeys},
        m(
            'main',
            m(Sidebar),
            m(Topbar, {
              omnibox: this.renderOmnibox(),
            }),
            m(Alerts),
            children,
            m(CookieConsent),
            m(fullscreenModalContainer.mithrilComponent),
            globals.state.perfDebug && m('.perf-stats'),
            ),
    );
  }

  oncreate({dom}: m.VnodeDOM) {
    const unreg = globals.commandManager.registerCommandSource(this);
    this.trash.add(unreg);

    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
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
      if (this.omniboxInputEl) {
        this.omniboxInputEl.focus();
        this.omniboxInputEl.select();
      }
      this.focusOmniboxNextRender = false;
    }
  }
}
