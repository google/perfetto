// Copyright (C) 2024 The Android Open Source Project
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
import {TrackSearchManager} from '../../core/track_search_manager';
import {Button} from '../../widgets/button';
import {Popup, PopupPosition} from '../../widgets/popup';
import {TextInput} from '../../widgets/text_input';
import {Tooltip} from '../../widgets/tooltip';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {Anchor} from '../../widgets/anchor';

export interface TrackSearchBarAttrs {
  readonly searchManager: TrackSearchManager;
}

/**
 * A search panel for searching tracks by name.
 * Displayed at the top of the timeline when active.
 */
export class TrackSearchBar implements m.ClassComponent<TrackSearchBarAttrs> {
  view({attrs}: m.Vnode<TrackSearchBarAttrs>): m.Children {
    const {searchManager} = attrs;

    return m(
      '.pf-track-search-bar',
      m(
        '.pf-track-search-bar__bubble',
        this.renderMatchCount(searchManager),
        this.renderHelpButton(searchManager),
        m(TextInput, {
          autofocus: true,
          placeholder: 'Search tracks...',
          value: searchManager.searchTerm,
          onInput: (value: string) => {
            searchManager.setSearchTerm(value);
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Escape') {
              e.preventDefault();
              searchManager.hide();
            } else if (e.key === 'Enter' && e.shiftKey) {
              e.preventDefault();
              searchManager.stepBackwards();
            } else if (e.key === 'Enter') {
              e.preventDefault();
              searchManager.stepForward();
            }
          },
        }),
        this.renderButtons(searchManager),
      ),
    );
  }

  onupdate({dom, attrs}: m.VnodeDOM<TrackSearchBarAttrs>) {
    // After the component updates, reset the focus flag so that it only triggers
    // on the first render after show() is called.
    if (attrs.searchManager.shouldFocus) {
      dom.querySelector('input')?.focus();
      attrs.searchManager.clearShouldFocus();
    }
  }

  private renderHelpButton(searchManager: TrackSearchManager) {
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
          "Virtual scrolling is active to improve performance, but it prevents the browser's find feature from seeing all tracks. Use this search bar instead, which supports regex and searching inside collapsed groups.",
        ),
        m(
          'p',
          'Alternatively, ',
          m(
            Anchor,
            {
              href: '#!/settings/virtualTrackScrolling',
              onclick: () => searchManager.hide(),
            },
            'disable virtual scrolling',
          ),
          ' (may decrease performance) or ',
          m(
            Anchor,
            {
              href: '#!/settings/alternativeSearchHotkey',
              onclick: () => searchManager.hide(),
            },
            'use Shift+F',
          ),
          ' for track search to keep ',
          m(HotkeyGlyphs, {hotkey: 'Mod+F'}),
          ' for the browser.',
        ),
      ),
    );
  }

  private renderButtons(searchManager: TrackSearchManager) {
    return m(
      '.pf-track-search-bar__buttons',
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: 'regular_expression',
            active: searchManager.useRegex,
            onclick: () => {
              searchManager.useRegex = !searchManager.useRegex;
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
            active: searchManager.searchCollapsed,
            onclick: () => {
              searchManager.searchCollapsed = !searchManager.searchCollapsed;
            },
          }),
        },
        'Search in collapsed groups',
      ),
      m(
        Tooltip,
        {
          position: PopupPosition.Top,
          trigger: m(Button, {
            icon: Icons.Up,
            disabled: searchManager.matchCount === 0,
            onclick: () => searchManager.stepBackwards(),
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
            disabled: searchManager.matchCount === 0,
            onclick: () => searchManager.stepForward(),
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
            onclick: () => searchManager.hide(),
          }),
        },
        'Close (Escape)',
      ),
    );
  }

  private renderMatchCount(searchManager: TrackSearchManager): m.Children {
    const count = searchManager.matchCount;
    const current = searchManager.currentMatchIndex;

    if (!searchManager.searchTerm) return null;

    const matchText = count === 0 ? 'No matches' : `${current + 1} of ${count}`;
    return m('.pf-track-search-bar__count', matchText);
  }
}
