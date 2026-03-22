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
import {Icons} from '../../base/semantic_icons';
import {Button} from '../../widgets/button';
import {Popup, PopupPosition} from '../../widgets/popup';
import {TextInput} from '../../widgets/text_input';
import {Tooltip} from '../../widgets/tooltip';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {Anchor} from '../../widgets/anchor';
import {TrackSearchModel} from '../../core/track_search_manager';

export interface TrackSearchBarApi {
  focus(): void;
}

export interface TrackSearchBarAttrs {
  readonly matchCount: number;
  readonly currentMatchIndex: number;
  readonly model: TrackSearchModel;
  onModelChange?(newModel: TrackSearchModel): void;
  onStepForward?(): void;
  onStepBackwards?(): void;
  onClose?(dom: EventTarget | null): void;
  onReady?(api: TrackSearchBarApi): void;
}

/**
 * A search panel for searching tracks by name.
 * Displayed at the top of the timeline when active.
 */
export class TrackSearchBar implements m.ClassComponent<TrackSearchBarAttrs> {
  view({attrs}: m.Vnode<TrackSearchBarAttrs>): m.Children {
    const {
      model,
      onModelChange,
      onStepBackwards,
      onStepForward,
      onClose,
      matchCount,
      currentMatchIndex,
    } = attrs;

    return m(
      '.pf-track-search-bar',
      m(
        '.pf-track-search-bar__bubble',
        model.searchTerm !== '' &&
          this.renderMatchCount(matchCount, currentMatchIndex),
        this.renderHelpButton(),
        m(TextInput, {
          autofocus: true,
          placeholder: 'Search tracks...',
          value: model.searchTerm,
          onInput: (value: string) => {
            onModelChange?.({
              ...model,
              searchTerm: value,
            });
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Escape') {
              e.preventDefault();
              onClose?.(e.target);
            } else if (e.key === 'Enter' && e.shiftKey) {
              e.preventDefault();
              onStepBackwards?.();
            } else if (e.key === 'Enter') {
              e.preventDefault();
              onStepForward?.();
            }
          },
        }),
        this.renderButtons(attrs),
      ),
    );
  }

  onupdate({dom, attrs}: m.VnodeDOM<TrackSearchBarAttrs>) {
    const {onReady} = attrs;

    if (onReady) {
      onReady({
        focus: () => {
          const input = dom.querySelector('input');
          input?.focus();
          input?.select();
        },
      });
    }
  }

  private renderHelpButton(): m.Children {
    return m(
      Popup,
      {
        position: PopupPosition.Top,
        trigger: m(Button, {
          icon: Icons.Help,
        }),
      },
      m(
        '.pf-track-search-help',
        m(
          'p',
          'Tracks use virtual scrolling — only visible tracks are rendered in the DOM. This means the native browser find (',
          m(HotkeyGlyphs, {hotkey: 'Mod+F'}),
          ") can't find off-screen tracks.",
        ),
        m(
          'p',
          'This search finds all tracks regardless of scroll position, supports regex, and can search within collapsed groups.',
        ),
        m(
          'p',
          'Prefer ',
          m(HotkeyGlyphs, {hotkey: 'Mod+F'}),
          '? ',
          m(
            Anchor,
            {
              href: '#!/settings/alternativeSearchHotkey',
            },
            'Rebind this to Shift+F',
          ),
          ', or ',
          m(
            Anchor,
            {
              href: '#!/settings/virtualTrackScrolling',
            },
            'disable virtual scrolling',
          ),
          ' (can drastically reduce timeline performance).',
        ),
      ),
    );
  }

  private renderButtons(attrs: TrackSearchBarAttrs) {
    const {
      onStepBackwards,
      onStepForward,
      onClose,
      matchCount,
      model,
      onModelChange,
    } = attrs;

    return m(
      '.pf-track-search-bar__buttons',
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: 'regular_expression',
            active: model.useRegex,
            onclick: () => {
              onModelChange?.({
                ...model,
                useRegex: !model.useRegex,
              });
            },
          }),
        },
        'Use regular expression',
      ),
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: 'account_tree',
            active: model.searchWithinCollapsedGroups,
            onclick: () => {
              onModelChange?.({
                ...model,
                searchWithinCollapsedGroups: !model.searchWithinCollapsedGroups,
              });
            },
          }),
        },
        'Search within collapsed groups',
      ),
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: Icons.Up,
            disabled: matchCount === 0,
            onclick: onStepBackwards,
          }),
        },
        'Previous match (Shift+Enter)',
      ),
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: Icons.Down,
            disabled: matchCount === 0,
            onclick: onStepForward,
          }),
        },
        'Next match (Enter)',
      ),
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: Icons.Close,
            onclick: (e) => onClose?.(e.target),
          }),
        },
        'Close (Escape)',
      ),
    );
  }

  private renderMatchCount(count: number, current: number): m.Children {
    const matchText = count === 0 ? 'No matches' : `${current + 1} of ${count}`;
    return m('.pf-track-search-bar__count', matchText);
  }
}
