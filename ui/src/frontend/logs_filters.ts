// Copyright (C) 2022 The Android Open Source Project
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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {globals} from './globals';

export const LOG_PRIORITIES =
    ['-', '-', 'Verbose', 'Debug', 'Info', 'Warn', 'Error', 'Fatal'];
const IGNORED_STATES = 2;

interface LogPriorityWidgetAttrs {
  options: string[];
  selectedIndex: number;
  onSelect: (id: number) => void;
}

interface LogTagChipAttrs {
  name: string;
  removeTag: (name: string) => void;
}

interface LogTagsWidgetAttrs {
  tags: string[];
}

interface FilterByTextWidgetAttrs {
  hideNonMatching: boolean;
}

class LogPriorityWidget implements m.ClassComponent<LogPriorityWidgetAttrs> {
  view(vnode: m.Vnode<LogPriorityWidgetAttrs>) {
    const attrs = vnode.attrs;
    const optionComponents = [];
    for (let i = IGNORED_STATES; i < attrs.options.length; i++) {
      const selected = i === attrs.selectedIndex;
      optionComponents.push(
          m('option', {value: i, selected}, attrs.options[i]));
    }
    return m(
        'select',
        {
          onchange: (e: InputEvent) => {
            const selectionValue = (e.target as HTMLSelectElement).value;
            attrs.onSelect(Number(selectionValue));
          },
        },
        optionComponents,
    );
  }
}

class LogTagChip implements m.ClassComponent<LogTagChipAttrs> {
  view({attrs}: m.CVnode<LogTagChipAttrs>) {
    return m(
        '.chip',
        m('.chip-text', attrs.name),
        m('button.chip-button',
          {
            onclick: () => {
              attrs.removeTag(attrs.name);
            },
          },
          '×'));
  }
}

class LogTagsWidget implements m.ClassComponent<LogTagsWidgetAttrs> {
  removeTag(tag: string) {
    globals.dispatch(Actions.removeLogTag({tag}));
  }

  view(vnode: m.Vnode<LogTagsWidgetAttrs>) {
    const tags = vnode.attrs.tags;
    return m(
        '.tag-container',
        m('.chips', tags.map((tag) => m(LogTagChip, {
                               name: tag,
                               removeTag: this.removeTag.bind(this),
                             }))),
        m(`input.chip-input[placeholder='Add new tag']`, {
          onkeydown: (e: KeyboardEvent) => {
            // This is to avoid zooming on 'w'(and other unexpected effects
            // of key presses in this input field).
            e.stopPropagation();
            const htmlElement = e.target as HTMLInputElement;

            // When the user clicks 'Backspace' we delete the previous tag.
            if (e.key === 'Backspace' && tags.length > 0 &&
                htmlElement.value === '') {
              globals.dispatch(
                  Actions.removeLogTag({tag: tags[tags.length - 1]}));
              return;
            }

            if (e.key !== 'Enter') {
              return;
            }
            if (htmlElement.value === '') {
              return;
            }
            globals.dispatch(
                Actions.addLogTag({tag: htmlElement.value.trim()}));
            htmlElement.value = '';
          },
        }));
  }
}

class LogTextWidget implements m.ClassComponent {
  view() {
    return m(
        '.tag-container', m(`input.chip-input[placeholder='Search log text']`, {
          onkeydown: (e: KeyboardEvent) => {
            // This is to avoid zooming on 'w'(and other unexpected effects
            // of key presses in this input field).
            e.stopPropagation();
          },

          onkeyup: (e: KeyboardEvent) => {
            // We want to use the value of the input field after it has been
            // updated with the latest key (onkeyup).
            const htmlElement = e.target as HTMLInputElement;
            globals.dispatch(
                Actions.updateLogFilterText({textEntry: htmlElement.value}));
          },
        }));
  }
}

class FilterByTextWidget implements m.ClassComponent<FilterByTextWidgetAttrs> {
  view({attrs}: m.Vnode<FilterByTextWidgetAttrs>) {
    const icon = attrs.hideNonMatching ? 'unfold_less' : 'unfold_more';
    const tooltip = attrs.hideNonMatching ? 'Expand all and view highlighted' :
                                            'Collapse all';
    return m(
        '.filter-widget',
        m('.tooltip', tooltip),
        m('i.material-icons',
          {
            onclick: () => {
              globals.dispatch(Actions.toggleCollapseByTextEntry({}));
            },
          },
          icon));
  }
}

export class LogsFilters implements m.ClassComponent {
  view(_: m.CVnode<{}>) {
    return m(
        '.log-filters',
        m('.log-label', 'Log Level'),
        m(LogPriorityWidget, {
          options: LOG_PRIORITIES,
          selectedIndex: globals.state.logFilteringCriteria.minimumLevel,
          onSelect: (minimumLevel) => {
            globals.dispatch(Actions.setMinimumLogLevel({minimumLevel}));
          },
        }),
        m(LogTagsWidget, {tags: globals.state.logFilteringCriteria.tags}),
        m(LogTextWidget),
        m(FilterByTextWidget, {
          hideNonMatching: globals.state.logFilteringCriteria.hideNonMatching,
        }));
  }
}
