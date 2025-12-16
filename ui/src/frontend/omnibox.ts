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
import {classNames} from '../base/classnames';
import {findRef} from '../base/dom_utils';
import {FuzzyFinder, FuzzySegment} from '../base/fuzzy';
import {assertExists, assertUnreachable} from '../base/logging';
import {isString} from '../base/object_utils';
import {undoCommonChatAppReplacements} from '../base/string_utils';
import {exists} from '../base/utils';
import {addQueryResultsTab} from '../components/query_table/query_result_tab';
import {AppImpl} from '../core/app_impl';
import {OmniboxMode} from '../core/omnibox_manager';
import {raf} from '../core/raf_scheduler';
import {TraceImpl} from '../core/trace_impl';
import {Button} from '../widgets/button';
import {Chip} from '../widgets/chip';
import {HTMLAttrs} from '../widgets/common';
import {EmptyState} from '../widgets/empty_state';
import {HotkeyGlyphs, KeycapGlyph} from '../widgets/hotkey_glyphs';
import {Popup} from '../widgets/popup';
import {Spinner} from '../widgets/spinner';

const OMNIBOX_INPUT_REF = 'omnibox';
const RECENT_COMMANDS_LIMIT = 6;

// Omnibox attrs - simplified to just what's needed from outside
export interface OmniboxAttrs {
  readonly trace?: TraceImpl;
}

// Omnibox: Smart component that contains all omnibox business logic
// and wraps the presentational OmniboxWidget
export class Omnibox implements m.ClassComponent<OmniboxAttrs> {
  private omniboxInputEl?: HTMLInputElement;
  private recentCommands: ReadonlyArray<string> = [];

  view({attrs}: m.Vnode<OmniboxAttrs>): m.Children {
    const {trace} = attrs;
    const app = AppImpl.instance;
    const omnibox = app.omnibox;
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

    return m(OmniboxWidget, {
      value: omnibox.text,
      placeholder: prompt.text,
      inputRef: OMNIBOX_INPUT_REF,
      className: 'pf-omnibox--prompt-mode',
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

    return m(OmniboxWidget, {
      value: omnibox.text,
      placeholder: 'Filter commands...',
      inputRef: OMNIBOX_INPUT_REF,
      className: 'pf-omnibox--command-mode',
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
    return m(OmniboxWidget, {
      value: AppImpl.instance.omnibox.text,
      placeholder: ph,
      inputRef: OMNIBOX_INPUT_REF,
      className: 'pf-omnibox--query-mode',

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
    return m(OmniboxWidget, {
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

  oncreate({dom}: m.VnodeDOM<OmniboxAttrs>) {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
  }

  onupdate({dom}: m.VnodeDOM<OmniboxAttrs>) {
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

interface OmniboxOptionRowAttrs extends HTMLAttrs {
  // Human readable display name for the option.
  // This can either be a simple string, or a list of fuzzy segments in which
  // case highlighting will be applied to the matching segments.
  readonly displayName: FuzzySegment[] | string;

  // Highlight this option.
  readonly highlighted: boolean;

  // Arbitrary components to put on the right hand side of the option.
  readonly rightContent?: m.Children;

  // Some tag to place on the right (to the left of the right content).
  readonly label?: string;
}

class OmniboxOptionRow implements m.ClassComponent<OmniboxOptionRowAttrs> {
  private highlightedBefore = false;

  view({attrs}: m.Vnode<OmniboxOptionRowAttrs>): void | m.Children {
    const {displayName, highlighted, rightContent, label, ...htmlAttrs} = attrs;
    return m(
      'li',
      {
        class: classNames(highlighted && 'pf-highlighted'),
        ...htmlAttrs,
      },
      m('span.pf-title', this.renderTitle(displayName)),
      label && m(Chip, {className: 'pf-omnibox__tag', label, rounded: true}),
      rightContent,
    );
  }

  private renderTitle(title: FuzzySegment[] | string): m.Children {
    if (isString(title)) {
      return title;
    } else {
      return title.map(({matching, value}) => {
        return matching ? m('b', value) : value;
      });
    }
  }

  onupdate({attrs, dom}: m.VnodeDOM<OmniboxOptionRowAttrs, this>) {
    if (this.highlightedBefore !== attrs.highlighted) {
      if (attrs.highlighted) {
        dom.scrollIntoView({block: 'nearest'});
      }
      this.highlightedBefore = attrs.highlighted;
    }
  }
}

// Omnibox option.
interface OmniboxOption {
  // The value to place into the omnibox. This is what's returned in onSubmit.
  readonly key: string;

  // Display name provided as a string or a list of fuzzy segments to enable
  // fuzzy match highlighting.
  readonly displayName: FuzzySegment[] | string;

  // Some tag to place on the right (to the left of the right content).
  readonly tag?: string;

  // Arbitrary components to put on the right hand side of the option.
  readonly rightContent?: m.Children;
}

interface OmniboxWidgetAttrs extends HTMLAttrs {
  // Current value of the omnibox input.
  readonly value: string;

  // What to show when value is blank.
  readonly placeholder?: string;

  // Called when the text changes.
  readonly onInput?: (value: string, previousValue: string) => void;

  // Called on close.
  readonly onClose?: () => void;

  // Dropdown items to show. If none are supplied, the omnibox runs in free text
  // mode, where anyt text can be input. Otherwise, onSubmit will always be
  // called with one of the options.
  // Options are provided in groups called categories. If the category has a
  // name the name will be listed at the top of the group rendered with a little
  // divider as well.
  readonly options?: OmniboxOption[];

  // Called when the user expresses the intent to "execute" the thing.
  readonly onSubmit?: (value: string, mod: boolean, shift: boolean) => void;

  // Called when the user hits backspace when the field is empty.
  readonly onGoBack?: () => void;

  // When true, disable and grey-out the omnibox's input.
  readonly readonly?: boolean;

  // Ref to use on the input - useful for extracing this element from the DOM.
  readonly inputRef?: string;

  // Whether to close when the user presses Enter. Default = false.
  readonly closeOnSubmit?: boolean;

  // Whether to close the omnibox (i.e. call the |onClose| handler) when we
  // click outside the omnibox or its dropdown. Default = false.
  readonly closeOnOutsideClick?: boolean;

  // Some content to place into the right hand side of the after the input.
  readonly rightContent?: m.Children;

  // If we have options, this value indicates the index of the option which
  // is currently highlighted.
  readonly selectedOptionIndex?: number;

  // Callback for when the user pressed up/down, expressing a desire to change
  // the |selectedOptionIndex|.
  readonly onSelectedOptionChanged?: (index: number) => void;
}

class OmniboxWidget implements m.ClassComponent<OmniboxWidgetAttrs> {
  private popupElement?: HTMLElement;
  private dom?: Element;
  private attrs?: OmniboxWidgetAttrs;

  view({attrs}: m.Vnode<OmniboxWidgetAttrs>): m.Children {
    const {
      value,
      placeholder,
      onInput = () => {},
      onSubmit = () => {},
      onGoBack = () => {},
      inputRef = 'omnibox',
      options,
      closeOnSubmit = false,
      rightContent,
      selectedOptionIndex = 0,
      ...htmlAttrs
    } = attrs;

    return m(
      Popup,
      {
        onPopupMount: (dom: HTMLElement) => (this.popupElement = dom),
        onPopupUnMount: (_dom: HTMLElement) => (this.popupElement = undefined),
        isOpen: exists(options),
        showArrow: false,
        matchWidth: true,
        offset: 2,
        trigger: m(
          '.pf-omnibox',
          htmlAttrs,
          m('input', {
            spellcheck: false,
            ref: inputRef,
            value,
            placeholder,
            oninput: (e: Event) => {
              onInput((e.target as HTMLInputElement).value, value);
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Backspace' && value === '') {
                onGoBack();
              } else if (e.key === 'Escape') {
                e.preventDefault();
                this.close(attrs);
              }

              if (options) {
                if (e.key === 'ArrowUp') {
                  e.preventDefault();
                  this.highlightPreviousOption(attrs);
                } else if (e.key === 'ArrowDown') {
                  e.preventDefault();
                  this.highlightNextOption(attrs);
                } else if (e.key === 'Enter') {
                  e.preventDefault();

                  const option = options[selectedOptionIndex];
                  // Return values from indexing arrays can be undefined.
                  // We should enable noUncheckedIndexedAccess in
                  // tsconfig.json.
                  /* eslint-disable
                      @typescript-eslint/strict-boolean-expressions */
                  if (option) {
                    /* eslint-enable */
                    closeOnSubmit && this.close(attrs);

                    const mod = e.metaKey || e.ctrlKey;
                    const shift = e.shiftKey;
                    onSubmit(option.key, mod, shift);
                  }
                }
              } else {
                if (e.key === 'Enter') {
                  e.preventDefault();

                  closeOnSubmit && this.close(attrs);

                  const mod = e.metaKey || e.ctrlKey;
                  const shift = e.shiftKey;
                  onSubmit(value, mod, shift);
                }
              }
            },
          }),
          rightContent,
        ),
      },
      options && this.renderDropdown(attrs),
    );
  }

  private renderDropdown(attrs: OmniboxWidgetAttrs): m.Children {
    const {options} = attrs;

    if (!options) return null;

    if (options.length === 0) {
      return m(EmptyState, {title: 'No matching options...'});
    } else {
      return m(
        '.pf-omnibox-dropdown',
        this.renderOptionsContainer(attrs, options),
        this.renderFooter(),
      );
    }
  }

  private renderFooter() {
    return m(
      '.pf-omnibox-dropdown-footer',
      m(
        'section',
        m(KeycapGlyph, {keyValue: 'ArrowUp'}),
        m(KeycapGlyph, {keyValue: 'ArrowDown'}),
        'to navigate',
      ),
      m('section', m(KeycapGlyph, {keyValue: 'Enter'}), 'to use'),
      m('section', m(KeycapGlyph, {keyValue: 'Escape'}), 'to dismiss'),
    );
  }

  private renderOptionsContainer(
    attrs: OmniboxWidgetAttrs,
    options: OmniboxOption[],
  ): m.Children {
    const {
      onClose = () => {},
      onSubmit = () => {},
      closeOnSubmit = false,
      selectedOptionIndex,
    } = attrs;

    const opts = options.map(({displayName, key, rightContent, tag}, index) => {
      return m(OmniboxOptionRow, {
        key,
        label: tag,
        displayName: displayName,
        highlighted: index === selectedOptionIndex,
        onclick: () => {
          closeOnSubmit && onClose();
          onSubmit(key, false, false);
        },
        rightContent,
      });
    });

    return m('ul.pf-omnibox-options-container', opts);
  }

  oncreate({attrs, dom}: m.VnodeDOM<OmniboxWidgetAttrs>) {
    this.attrs = attrs;
    this.dom = dom;
    const {closeOnOutsideClick} = attrs;
    if (closeOnOutsideClick) {
      document.addEventListener('mousedown', this.onMouseDown);
    }
  }

  onupdate({attrs, dom}: m.VnodeDOM<OmniboxWidgetAttrs>) {
    this.attrs = attrs;
    this.dom = dom;
    const {closeOnOutsideClick} = attrs;
    if (closeOnOutsideClick) {
      document.addEventListener('mousedown', this.onMouseDown);
    } else {
      document.removeEventListener('mousedown', this.onMouseDown);
    }
  }

  onremove() {
    this.attrs = undefined;
    this.dom = undefined;
    document.removeEventListener('mousedown', this.onMouseDown);
  }

  // This is defined as an arrow function to have a single handler that can be
  // added/remove while keeping `this` bound.
  private onMouseDown = (e: Event) => {
    // We need to schedule a redraw manually as this event handler was added
    // manually to the DOM and doesn't use Mithril's auto-redraw system.
    raf.scheduleFullRedraw();

    // Don't close if the click was within ourselves or our popup.
    if (e.target instanceof Node) {
      if (this.popupElement && this.popupElement.contains(e.target)) {
        return;
      }
      if (this.dom && this.dom.contains(e.target)) return;
    }
    if (this.attrs) {
      this.close(this.attrs);
    }
  };

  private close(attrs: OmniboxWidgetAttrs): void {
    const {onClose = () => {}} = attrs;
    onClose();
  }

  private highlightPreviousOption(attrs: OmniboxWidgetAttrs) {
    const {selectedOptionIndex = 0, onSelectedOptionChanged = () => {}} = attrs;

    onSelectedOptionChanged(Math.max(0, selectedOptionIndex - 1));
  }

  private highlightNextOption(attrs: OmniboxWidgetAttrs) {
    const {
      selectedOptionIndex = 0,
      onSelectedOptionChanged = () => {},
      options = [],
    } = attrs;

    const max = options.length - 1;
    onSelectedOptionChanged(Math.min(max, selectedOptionIndex + 1));
  }
}
