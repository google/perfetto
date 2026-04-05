// Copyright (C) 2026 The Android Open Source Project
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
import {classNames} from '../../base/classnames';
import {findRef} from '../../base/dom_utils';
import {FuzzyFinder, FuzzySegment} from '../../base/fuzzy';
import {assertExists, assertUnreachable} from '../../base/assert';
import {isString} from '../../base/object_utils';
import {exists} from '../../base/utils';
import {OmniboxMode} from '../../core/omnibox_manager';
import {raf} from '../../core/raf_scheduler';
import {Chip} from '../../widgets/chip';
import {HTMLAttrs, Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {HotkeyGlyphs, KeycapGlyph} from '../../widgets/hotkey_glyphs';
import {Popup} from '../../widgets/popup';
import {BigTraceApp} from '../bigtrace_app';

const OMNIBOX_INPUT_REF = 'omnibox';
const RECENT_COMMANDS_LIMIT = 6;

// Smart omnibox component for BigTrace. Mirrors ui/src/frontend/omnibox.ts but
// uses BigTraceApp instead of AppImpl and omits trace-search step-through.
export class Omnibox implements m.ClassComponent {
  private omniboxInputEl?: HTMLInputElement;
  private recentCommands: ReadonlyArray<string> = [];

  view(): m.Children {
    const omnibox = BigTraceApp.instance.omnibox;
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
    } else if (omniboxMode === OmniboxMode.Search) {
      return this.renderSearchOmnibox();
    } else if (omniboxMode === OmniboxMode.RegisteredMode) {
      return this.renderRegisteredMode();
    } else {
      assertUnreachable(omniboxMode);
    }
  }

  private renderPromptOmnibox(): m.Children {
    const omnibox = BigTraceApp.instance.omnibox;
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
    const {commands, omnibox} = BigTraceApp.instance;
    const filteredCmds = commands.fuzzyFilterCommands(omnibox.text);

    const commandsWithHeuristics = filteredCmds.map((cmd) => {
      return {
        recentsIndex: this.recentCommands.findIndex((id) => id === cmd.id),
        cmd,
      };
    });

    const sorted = commandsWithHeuristics.sort((a, b) => {
      if (b.recentsIndex === a.recentsIndex) {
        return 0;
      } else {
        return b.recentsIndex - a.recentsIndex;
      }
    });

    const options = sorted.map(({recentsIndex, cmd}): OmniboxOption => {
      const {segments, id, defaultHotkey, source} = cmd;
      return {
        key: id,
        displayName: segments,
        tag: recentsIndex !== -1 ? 'recently used' : undefined,
        source,
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
      .filter((x) => x !== id)
      .concat(id)
      .splice(-RECENT_COMMANDS_LIMIT);
  }

  private renderRegisteredMode(): m.Children {
    const omnibox = BigTraceApp.instance.omnibox;
    const desc = assertExists(omnibox.activeRegisteredMode);
    return m(OmniboxWidget, {
      value: omnibox.text,
      placeholder: desc.placeholder,
      inputRef: OMNIBOX_INPUT_REF,
      className: desc.className,
      onInput: (value) => {
        if (desc.onInput) {
          desc.onInput(value);
        } else {
          omnibox.setText(value);
        }
      },
      onSubmit: (value, alt) => {
        desc.onSubmit(value, alt);
      },
      onClose: () => {
        if (desc.onClose) {
          desc.onClose();
        } else {
          omnibox.setText('');
          if (this.omniboxInputEl) {
            this.omniboxInputEl.blur();
          }
          omnibox.reset();
        }
      },
      onGoBack: () => {
        if (desc.onGoBack) {
          desc.onGoBack();
        } else {
          omnibox.reset();
        }
      },
    });
  }

  private renderSearchOmnibox(): m.Children {
    const omnibox = BigTraceApp.instance.omnibox;
    const hints = ["'>' for commands"];
    for (const desc of omnibox.registeredModes.values()) {
      if (desc.hint) hints.push(desc.hint);
    }
    return m(OmniboxWidget, {
      value: omnibox.text,
      placeholder: `Search or type ${hints.join(', ')}`,
      inputRef: OMNIBOX_INPUT_REF,
      onInput: (value, _prev) => {
        if (value === '>') {
          omnibox.setMode(OmniboxMode.Command);
          return;
        }
        if (value.length === 1 && omnibox.registeredModes.has(value)) {
          omnibox.activateRegisteredMode(value);
          return;
        }
        omnibox.setText(value);
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (_value, _mod, _shift) => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
    });
  }

  oncreate({dom}: m.VnodeDOM): void {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
  }

  onupdate({dom}: m.VnodeDOM): void {
    this.updateOmniboxInputRef(dom);
    this.maybeFocusOmnibar();
  }

  private updateOmniboxInputRef(dom: Element): void {
    const el = findRef(dom, OMNIBOX_INPUT_REF);
    if (el && el instanceof HTMLInputElement) {
      this.omniboxInputEl = el;
    }
  }

  private maybeFocusOmnibar(): void {
    if (BigTraceApp.instance.omnibox.focusOmniboxNextRender) {
      const omniboxEl = this.omniboxInputEl;
      if (omniboxEl) {
        omniboxEl.focus();
        if (BigTraceApp.instance.omnibox.pendingCursorPlacement === undefined) {
          omniboxEl.select();
        } else {
          omniboxEl.setSelectionRange(
            BigTraceApp.instance.omnibox.pendingCursorPlacement,
            BigTraceApp.instance.omnibox.pendingCursorPlacement,
          );
        }
      }
      BigTraceApp.instance.omnibox.clearFocusFlag();
    }
  }
}

// ---------------------------------------------------------------------------
// Presentational widget layer (mirrors ui/src/frontend/omnibox.ts)
// ---------------------------------------------------------------------------

interface OmniboxOptionRowAttrs extends HTMLAttrs {
  readonly displayName: FuzzySegment[] | string;
  readonly highlighted: boolean;
  readonly rightContent?: m.Children;
  readonly label?: string;
  readonly source?: string;
}

class OmniboxOptionRow implements m.ClassComponent<OmniboxOptionRowAttrs> {
  private highlightedBefore = false;

  view({attrs}: m.Vnode<OmniboxOptionRowAttrs>): void | m.Children {
    const {
      displayName,
      highlighted,
      rightContent,
      label,
      source,
      ...htmlAttrs
    } = attrs;
    return m(
      'li',
      {
        class: classNames(highlighted && 'pf-highlighted'),
        ...htmlAttrs,
      },
      source &&
        m(Chip, {
          className: 'pf-omnibox__source',
          label: source,
          rounded: true,
          compact: true,
          intent: Intent.Primary,
        }),
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

interface OmniboxOption {
  readonly key: string;
  readonly displayName: FuzzySegment[] | string;
  readonly tag?: string;
  readonly source?: string;
  readonly rightContent?: m.Children;
}

interface OmniboxWidgetAttrs extends HTMLAttrs {
  readonly value: string;
  readonly placeholder?: string;
  readonly onInput?: (value: string, previousValue: string) => void;
  readonly onClose?: () => void;
  readonly options?: OmniboxOption[];
  readonly onSubmit?: (value: string, mod: boolean, shift: boolean) => void;
  readonly onGoBack?: () => void;
  readonly readonly?: boolean;
  readonly inputRef?: string;
  readonly closeOnSubmit?: boolean;
  readonly closeOnOutsideClick?: boolean;
  readonly rightContent?: m.Children;
  readonly selectedOptionIndex?: number;
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

    const opts = options.map(
      ({displayName, key, rightContent, tag, source}, index) => {
        return m(OmniboxOptionRow, {
          key,
          label: tag,
          source,
          displayName: displayName,
          highlighted: index === selectedOptionIndex,
          onclick: () => {
            closeOnSubmit && onClose();
            onSubmit(key, false, false);
          },
          rightContent,
        });
      },
    );

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

  // Defined as an arrow function to keep `this` bound when used as an event
  // listener that is added/removed manually.
  private onMouseDown = (e: Event) => {
    raf.scheduleFullRedraw();

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
