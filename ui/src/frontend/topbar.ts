// Copyright (C) 2018 The Android Open Source Project
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
import {classNames} from '../base/classnames';
import {findRef} from '../base/dom_utils';
import {FuzzyFinder} from '../base/fuzzy';
import {assertExists, assertFalse, assertUnreachable} from '../base/logging';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {addQueryResultsTab} from '../components/query_table/query_result_tab';
import {AppImpl} from '../core/app_impl';
import {OmniboxManagerImpl, OmniboxMode} from '../core/omnibox_manager';
import {Router} from '../core/router';
import {TraceImpl, TraceImplAttrs} from '../core/trace_impl';
import {Button} from '../widgets/button';
import {Intent} from '../widgets/common';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {Popup, PopupPosition} from '../widgets/popup';
import {Spinner} from '../widgets/spinner';
import {Omnibox, OmniboxOption} from './omnibox';

const OMNIBOX_INPUT_REF = 'omnibox';
const RECENT_COMMANDS_LIMIT = 6;

class TraceErrorIcon implements m.ClassComponent<TraceImplAttrs> {
  private tracePopupErrorDismissed = false;

  view({attrs}: m.CVnode<TraceImplAttrs>) {
    const trace = attrs.trace;
    const app = AppImpl.instance;

    if (app.embeddedMode) return;

    const mode = app.omnibox.mode;
    const totErrors = trace.traceInfo.importErrors + trace.loadingErrors.length;
    if (totErrors === 0 || mode === OmniboxMode.Command) {
      return;
    }
    const message = Boolean(totErrors)
      ? `${totErrors} import or data loss errors detected.`
      : `Metric error detected.`;
    return m(
      '.pf-topbar__error-box',
      m(
        Popup,
        {
          trigger: m('span'),
          isOpen: !this.tracePopupErrorDismissed,
          position: PopupPosition.Left,
          onChange: (shouldOpen) => {
            assertFalse(shouldOpen);
            this.tracePopupErrorDismissed = true;
          },
        },
        m(
          '.pf-topbar__error-popup',
          'Data-loss/import error. Click for more info.',
        ),
      ),
      m(Button, {
        icon: 'announcement',
        title: message + ` Click for more info.`,
        intent: Intent.Danger,
        onclick: () => {
          // Navigate to the info page when the button is clicked.
          Router.navigate('#!/info');
        },
      }),
    );
  }
}

export interface TopbarAttrs {
  readonly trace?: TraceImpl;
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  private omniboxInputEl?: HTMLInputElement;
  private recentCommands: ReadonlyArray<string> = [];

  view({attrs}: m.Vnode<TopbarAttrs>) {
    const {trace} = attrs;
    const app = AppImpl.instance;
    return m(
      '.pf-topbar',
      {
        className: classNames(
          !app.sidebar.visible && 'pf-topbar--hide-sidebar',
        ),
      },
      this.renderOmnibox(trace, app.omnibox),
      trace && m(TraceErrorIcon, {trace}),
    );
  }

  private renderOmnibox(
    trace: TraceImpl | undefined,
    omnibox: OmniboxManagerImpl,
  ): m.Children {
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
      return this.renderQueryOmnibox(trace);
    } else if (omniboxMode === OmniboxMode.Search) {
      return this.renderSearchOmnibox(trace);
    } else {
      assertUnreachable(omniboxMode);
    }
  }

  private renderPromptOmnibox(): m.Children {
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

  private renderCommandOmnibox(): m.Children {
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
    this.recentCommands = this.recentCommands
      .filter((x) => x !== id) // Remove duplicates
      .concat(id) // Add to the end
      .splice(-RECENT_COMMANDS_LIMIT); // Limit items
  }

  private renderQueryOmnibox(trace: TraceImpl | undefined): m.Children {
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
        if (trace === undefined) return;
        addQueryResultsTab(trace, config, tag);
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

  private renderSearchOmnibox(trace: TraceImpl | undefined): m.Children {
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
        if (trace === undefined) return; // No trace loaded.
        if (value.length >= 4) {
          trace.search.search(value);
        } else {
          trace.search.reset();
        }
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (value, _mod, shift) => {
        if (trace === undefined) return; // No trace loaded.
        trace.search.search(value);
        if (shift) {
          trace.search.stepBackwards();
        } else {
          trace.search.stepForward();
        }
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      rightContent: trace && this.renderStepThrough(trace),
    });
  }

  private renderStepThrough(trace: TraceImpl) {
    const children = [];
    const results = trace.search.searchResults;
    if (trace?.search.searchInProgress) {
      children.push(m('.pf-omnibox__stepthrough-current', m(Spinner)));
    } else if (results !== undefined) {
      const searchMgr = trace.search;
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

  oncreate({dom}: m.VnodeDOM<TopbarAttrs>) {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
  }

  onupdate({dom}: m.VnodeDOM<TopbarAttrs>) {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
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
