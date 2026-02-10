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
import {PopupPosition} from '../../widgets/popup';
import {TextInput} from '../../widgets/text_input';
import {Tooltip} from '../../widgets/tooltip';

export interface TrackSearchPanelAttrs {
  readonly searchManager: TrackSearchManager;
}

/**
 * A search panel for searching tracks by name.
 * Displayed at the top of the timeline when active.
 */
export class TrackSearchPanel
  implements m.ClassComponent<TrackSearchPanelAttrs>
{
  view({attrs}: m.Vnode<TrackSearchPanelAttrs>): m.Children {
    const {searchManager} = attrs;

    return m(
      '.pf-track-search-panel',
      m('.pf-track-search-panel__count', this.renderMatchCount(searchManager)),
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
      m(
        '.pf-track-search-panel__buttons',
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
      ),
    );
  }

  private renderMatchCount(searchManager: TrackSearchManager): string {
    const count = searchManager.matchCount;
    const current = searchManager.currentMatchIndex;

    if (!searchManager.searchTerm) {
      return '';
    }

    if (count === 0) {
      return 'No matches';
    }

    return `${current + 1} of ${count}`;
  }
}
