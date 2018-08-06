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

import * as m from 'mithril';
import { globals } from './globals';
import { quietDispatch } from './mithril_helpers';
import { navigate } from '../common/actions';

let selResult = 0;
let numResults = 0;
let mode: 'search'|'command' = 'search';

function clearOmniboxResults() {
  // TODO(primiano): Implement in next CLs.
}

function onKeyDown(e: Event) {
  e.stopPropagation();
  const key = (e as KeyboardEvent).key;

  // Avoid that the global 'a', 'd', 'w', 's' handler sees these keystrokes.
  // TODO: this seems a bug in the pan_and_zoom_handler.ts.
  if (key === 'ArrowUp' || key === 'ArrowDown') {
    e.preventDefault();
    return;
  }
  const txt =
      (e.target as HTMLElement).querySelector('input') as HTMLInputElement;
  if (key === ':' && txt.value === '') {
    mode = 'command';
    m.redraw();
    e.preventDefault();
    return;
  }
  if (key === 'Escape' && mode === 'command') {
    txt.value = '';
    mode = 'search';
    m.redraw();
    return;
  }
  if (key === 'Backspace' && txt.value.length === 0 && mode === 'command') {
    mode = 'search';
    m.redraw();
    return;
  }
  // TODO(primiano): add query handling here.
}

function onKeyUp(e: Event) {
  e.stopPropagation();
  const key = (e as KeyboardEvent).key;
  const txt = e.target as HTMLInputElement;
  if (key === 'ArrowUp' || key === 'ArrowDown') {
    selResult += (key === 'ArrowUp') ? -1 : 1;
    selResult = Math.max(selResult, 0);
    selResult = Math.min(selResult, numResults - 1);
    e.preventDefault();
    m.redraw();
    return;
  }
  if (txt.value.length <= 0 || key === 'Escape') {
    clearOmniboxResults();
    m.redraw();
    return;
  }
  // TODO(primiano): add query handling here.
}


const Omnibox: m.Component = {
  oncreate(vnode) {
    const txt = vnode.dom.querySelector('input') as HTMLInputElement;
    txt.addEventListener('blur', clearOmniboxResults);
    txt.addEventListener('keydown', onKeyDown);
    txt.addEventListener('keyup', onKeyUp);
  },
  view() {
    // TODO(primiano): handle query results here.
    const placeholder = {
      search: 'Search or type : to enter command mode',
      command: 'e.g., select * from sched left join thread using(utid) limit 10'
    };
    const commandMode = mode === 'command';
    return m(
        `.omnibox${commandMode ? '.command-mode' : ''}`,
        m(`input[type=text][placeholder=${placeholder[mode]}]`));
  },
};

export const Topbar: m.Component = {
  view() {
    return m('.topbar', m(Omnibox));
  },
};