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

import {Actions} from '../common/actions';
import {EngineConfig} from '../common/state';

import {globals} from './globals';
import {executeSearch} from './search_handler';
import {taskTracker} from './task_tracker';

const SEARCH = Symbol('search');
const COMMAND = Symbol('command');
type Mode = typeof SEARCH|typeof COMMAND;

const PLACEHOLDER = {
  [SEARCH]: 'Search',
  [COMMAND]: 'e.g. select * from sched left join thread using(utid) limit 10'
};

export const DISMISSED_PANNING_HINT_KEY = 'dismissedPanningHint';

let mode: Mode = SEARCH;
let displayStepThrough = false;

function onKeyDown(e: Event) {
  const event = (e as KeyboardEvent);
  const key = event.key;
  if (key !== 'Enter') {
    e.stopPropagation();
  }
  const txt = (e.target as HTMLInputElement);

  if (mode === SEARCH && txt.value === '' && key === ':') {
    e.preventDefault();
    mode = COMMAND;
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }

  if (mode === COMMAND && txt.value === '' && key === 'Backspace') {
    mode = SEARCH;
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }

  if (mode === SEARCH && key === 'Enter') {
    txt.blur();
  }
}

function onKeyUp(e: Event) {
  e.stopPropagation();
  const event = (e as KeyboardEvent);
  const key = event.key;
  const txt = e.target as HTMLInputElement;

  if (key === 'Escape') {
    globals.dispatch(Actions.deleteQuery({queryId: 'command'}));
    mode = SEARCH;
    txt.value = '';
    txt.blur();
    globals.rafScheduler.scheduleFullRedraw();
    return;
  }
  if (mode === COMMAND && key === 'Enter') {
    globals.dispatch(Actions.executeQuery(
        {engineId: '0', queryId: 'command', query: txt.value}));
  }
}

class Omnibox implements m.ClassComponent {
  oncreate(vnode: m.VnodeDOM) {
    const txt = vnode.dom.querySelector('input') as HTMLInputElement;
    txt.addEventListener('keydown', onKeyDown);
    txt.addEventListener('keyup', onKeyUp);
  }

  view() {
    const msgTTL = globals.state.status.timestamp + 1 - Date.now() / 1e3;
    let enginesAreBusy = false;
    for (const engine of Object.values(globals.state.engines)) {
      enginesAreBusy = enginesAreBusy || !engine.ready;
    }

    if (msgTTL > 0 || enginesAreBusy) {
      setTimeout(
          () => globals.rafScheduler.scheduleFullRedraw(), msgTTL * 1000);
      return m(
          `.omnibox.message-mode`,
          m(`input[placeholder=${globals.state.status.msg}][readonly]`, {
            value: '',
          }));
    }

    const commandMode = mode === COMMAND;
    const state = globals.frontendLocalState;
    return m(
        `.omnibox${commandMode ? '.command-mode' : ''}`,
        m('input', {
          placeholder: PLACEHOLDER[mode],
          oninput: (e: InputEvent) => {
            const value = (e.target as HTMLInputElement).value;
            globals.frontendLocalState.setOmnibox(
                value, commandMode ? 'COMMAND' : 'SEARCH');
            if (mode === SEARCH) {
              globals.frontendLocalState.setSearchIndex(-1);
              displayStepThrough = value.length >= 4;
              globals.rafScheduler.scheduleFullRedraw();
            }
          },
          value: globals.frontendLocalState.omnibox,
        }),
        displayStepThrough ?
            m(
                '.stepthrough',
                m('.current',
                  `${
                      globals.currentSearchResults.totalResults === 0 ?
                          '0 / 0' :
                          `${state.searchIndex + 1} / ${
                              globals.currentSearchResults.totalResults}`}`),
                m('button',
                  {
                    disabled: state.searchIndex <= 0,
                    onclick: () => {
                      executeSearch(true /* reverse direction */);
                    }
                  },
                  m('i.material-icons.left', 'keyboard_arrow_left')),
                m('button',
                  {
                    disabled: state.searchIndex ===
                        globals.currentSearchResults.totalResults - 1,
                    onclick: () => {
                      executeSearch();
                    }
                  },
                  m('i.material-icons.right', 'keyboard_arrow_right')),
                ) :
            '');
  }
}

class Progress implements m.ClassComponent {
  private loading: () => void;
  private progressBar?: HTMLElement;

  constructor() {
    this.loading = () => this.loadingAnimation();
  }

  oncreate(vnodeDom: m.CVnodeDOM) {
    this.progressBar = vnodeDom.dom as HTMLElement;
    globals.rafScheduler.addRedrawCallback(this.loading);
  }

  onremove() {
    globals.rafScheduler.removeRedrawCallback(this.loading);
  }

  view() {
    return m('.progress');
  }

  loadingAnimation() {
    if (this.progressBar === undefined) return;
    const engine: EngineConfig = globals.state.engines['0'];
    if ((engine !== undefined && !engine.ready) ||
        globals.numQueuedQueries > 0 || taskTracker.hasPendingTasks()) {
      this.progressBar.classList.add('progress-anim');
    } else {
      this.progressBar.classList.remove('progress-anim');
    }
  }
}


class NewVersionNotification implements m.ClassComponent {
  view() {
    const engine: EngineConfig = globals.state.engines['0'];
    // Don't show the new version toast if a trace is loading (engine exists).
    if (!globals.frontendLocalState.newVersionAvailable ||
        engine !== undefined) {
      return;
    }
    return m(
        '.new-version-toast',
        'A new version of the UI is available!',
        m('button.notification-btn.preferred',
          {
            onclick: () => {
              location.reload();
            }
          },
          'Reload'),
        m('button.notification-btn',
          {
            onclick: () => {
              globals.frontendLocalState.newVersionAvailable = false;
              globals.rafScheduler.scheduleFullRedraw();
            }
          },
          'Dismiss'),
    );
  }
}


class HelpPanningNotification implements m.ClassComponent {
  view() {
    const dismissed = localStorage.getItem(DISMISSED_PANNING_HINT_KEY);
    if (dismissed === 'true' || !globals.frontendLocalState.showPanningHint) {
      return;
    }
    return m(
        '.helpful-hint',
        m('.hint-text',
          'Are you trying to pan? Use the WASD keys or hold shift to click ' +
              'and drag. Press \'?\' for more help.'),
        m('button.hint-dismiss-button',
          {
            onclick: () => {
              globals.frontendLocalState.showPanningHint = false;
              localStorage.setItem(DISMISSED_PANNING_HINT_KEY, 'true');
              globals.rafScheduler.scheduleFullRedraw();
            }
          },
          'Dismiss'),
    );
  }
}

class TraceErrorIcon implements m.ClassComponent {
  view() {
    const errors = globals.traceErrors;
    if (!errors && !globals.metricError || mode === COMMAND) return;
    const message = errors ? `${errors} import or data loss errors detected.` :
                             `Metric error detected.`;
    return m(
        'a.error',
        {href: '#!/info'},
        m('i.material-icons',
          {
            title: message + ` Click for more info.`,
          },
          'announcement'));
  }
}

export class Topbar implements m.ClassComponent {
  view() {
    return m(
        '.topbar',
        {
          class: globals.frontendLocalState.sidebarVisible ? '' : 'hide-sidebar'
        },
        globals.frontendLocalState.newVersionAvailable ?
            m(NewVersionNotification) :
            m(Omnibox),
        m(Progress),
        m(HelpPanningNotification),
        m(TraceErrorIcon));
  }
}
