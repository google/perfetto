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
import {DisposableStack} from '../base/disposable_stack';
import {findRef} from '../base/dom_utils';
import {FuzzyFinder} from '../base/fuzzy';
import {assertExists, assertUnreachable} from '../base/logging';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {addQueryResultsTab} from '../components/query_table/query_result_tab';
import {AppImpl} from '../core/app_impl';
import {CookieConsent} from '../core/cookie_consent';
import {featureFlags} from '../core/feature_flags';
import {OmniboxMode} from '../core/omnibox_manager';
import {TraceImpl} from '../core/trace_impl';
import {Command} from '../public/command';
import {Button} from '../widgets/button';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {LinearProgress} from '../widgets/linear_progress';
import {maybeRenderFullscreenModalDialog} from '../widgets/modal';
import {Spinner} from '../widgets/spinner';
import {initCssConstants} from './css_constants';
import {toggleHelp} from './help_modal';
import {Omnibox, OmniboxOption} from './omnibox';
import {Sidebar} from './sidebar';
import {renderStatusBar} from './statusbar';
import {taskTracker} from './task_tracker';
import {Topbar} from './topbar';

const showStatusBarFlag = featureFlags.register({
  id: 'Enable status bar',
  description: 'Enable status bar at the bottom of the window',
  defaultValue: true,
});

const OMNIBOX_INPUT_REF = 'omnibox';

// This wrapper creates a new instance of UiMainPerTrace for each new trace
// loaded (including the case of no trace at the beginning).
export class UiMain implements m.ClassComponent {
  oncreate({dom}: m.CVnodeDOM) {
    initCssConstants(dom);
  }
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
        id: 'dev.perfetto.OpenCommandPalette',
        name: 'Open command palette',
        callback: () => app.omnibox.setMode(OmniboxMode.Command),
        defaultHotkey: '!Mod+Shift+P',
      },

      {
        id: 'dev.perfetto.ShowHelp',
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
  }

  private renderOmnibox(): m.Children {
    const omnibox = AppImpl.instance.omnibox;
    const omniboxMode = omnibox.mode;
    const statusMessage = omnibox.statusMessage;
    if (statusMessage !== undefined) {
      return m(
        `.pf-omnibox.pf-omnibox--message-mode`,
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
      extraClasses: 'pf-omnibox--prompt-mode',
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
      extraClasses: 'pf-omnibox--command-mode',
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
      extraClasses: 'pf-omnibox--query-mode',

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
      children.push(m('.pf-omnibox__stepthrough-current', m(Spinner)));
    } else if (results !== undefined) {
      const searchMgr = assertExists(this.trace).search;
      const index = searchMgr.resultIndex;
      const total = results.totalResults ?? 0;
      children.push(
        m(
          '.pf-omnibox__stepthrough-current',
          `${total === 0 ? '0 / 0' : `${index + 1} / ${total}`}`,
        ),
        m(Button, {
          onclick: () => searchMgr.stepBackwards(),
          icon: 'keyboard_arrow_left',
        }),
        m(Button, {
          onclick: () => searchMgr.stepForward(),
          icon: 'keyboard_arrow_right',
        }),
      );
    }
    return m('.pf-omnibox__stepthrough', children);
  }

  oncreate(vnode: m.VnodeDOM) {
    this.updateOmniboxInputRef(vnode.dom);
    this.maybeFocusOmnibar();
  }

  view(): m.Children {
    const app = AppImpl.instance;
    const isSomethingLoading =
      AppImpl.instance.isLoadingTrace ||
      (this.trace?.engine.numRequestsPending ?? 0) > 0 ||
      taskTracker.hasPendingTasks();

    return m('main.pf-ui-main', [
      m(Sidebar, {trace: this.trace}),
      m(Topbar, {
        omnibox: this.renderOmnibox(),
        trace: this.trace,
      }),
      m(LinearProgress, {
        className: 'pf-ui-main__loading',
        state: isSomethingLoading ? 'indeterminate' : 'none',
      }),
      m('.pf-ui-main__page-container', app.pages.renderPageForCurrentRoute()),
      m(CookieConsent),
      maybeRenderFullscreenModalDialog(),
      showStatusBarFlag.get() && renderStatusBar(app.trace),
      app.perfDebugging.renderPerfStats(),
    ]);
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
}
