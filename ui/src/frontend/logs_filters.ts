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

export class LogsFilters implements m.ClassComponent {
  view(_: m.CVnode<{}>) {
    return m(
        '.log-filters', m('.log-label', 'Log Level'), m(LogPriorityWidget, {
          options: LOG_PRIORITIES,
          selectedIndex: globals.state.logFilteringCriteria.minimumLevel,
          onSelect: (minimumLevel) => {
            globals.dispatch(Actions.setMinimumLogLevel({minimumLevel}));
          },
        }));
  }
}
