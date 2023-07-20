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
import {CommandWithMatchInfo} from 'src/common/commands';

import {FuzzySegment} from '../base/fuzzy';
import {Actions} from '../common/actions';
import {raf} from '../core/raf_scheduler';
import {VERSION} from '../gen/perfetto_version';

import {classNames} from './classnames';
import {globals} from './globals';
import {runQueryInNewTab} from './query_result_tab';
import {executeSearch} from './search_handler';
import {taskTracker} from './task_tracker';
import {EmptyState} from './widgets/empty_state';
import {Icon} from './widgets/icon';
import {Popup} from './widgets/popup';

const SEARCH = Symbol('search');
const QUERY = Symbol('query');
const COMMAND = Symbol('command');
type Mode = typeof SEARCH|typeof QUERY|typeof COMMAND;
let highlightedCommandIndex = 0;

const PLACEHOLDER = {
  [SEARCH]: 'Search',
  [QUERY]: 'e.g. select * from sched left join thread using(utid) limit 10',
  [COMMAND]: 'Start typing a command..',
};

let matchingCommands: CommandWithMatchInfo[] = [];

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
    mode = QUERY;
    raf.scheduleFullRedraw();
    return;
  }

  if (mode === SEARCH && txt.value === '' && key === '>') {
    e.preventDefault();
    mode = COMMAND;
    raf.scheduleFullRedraw();
    return;
  }

  if (mode !== SEARCH && txt.value === '' && key === 'Backspace') {
    mode = SEARCH;
    raf.scheduleFullRedraw();
    return;
  }

  if (mode === COMMAND) {
    if (key === 'ArrowDown') {
      highlightedCommandIndex++;
      highlightedCommandIndex =
          Math.min(matchingCommands.length - 1, highlightedCommandIndex);
      raf.scheduleFullRedraw();
    } else if (key === 'ArrowUp') {
      highlightedCommandIndex--;
      highlightedCommandIndex = Math.max(0, highlightedCommandIndex);
      raf.scheduleFullRedraw();
    } else if (key === 'Enter') {
      const cmd = matchingCommands[highlightedCommandIndex];
      if (cmd) {
        globals.commandManager.runCommand(cmd.id);
      }
      highlightedCommandIndex = 0;
      mode = SEARCH;
      globals.dispatch(Actions.setOmnibox({
        omnibox: '',
        mode: 'SEARCH',
      }));
    } else {
      highlightedCommandIndex = 0;
    }
  }

  if (mode === SEARCH && key === 'Enter') {
    globals.dispatch(Actions.setOmnibox({
      omnibox: txt.value,
      mode: 'SEARCH',
      force: true,
    }));
    txt.blur();
  }

  if (mode === QUERY && key === 'Enter') {
    const openInPinnedTab = event.metaKey || event.ctrlKey;
    runQueryInNewTab(
        txt.value,
        openInPinnedTab ? 'Pinned query' : 'Omnibox query',
        openInPinnedTab ? undefined : 'omnibox_query',
    );
  }
}

function onKeyUp(e: Event) {
  e.stopPropagation();
  const event = (e as KeyboardEvent);
  const key = event.key;
  const txt = e.target as HTMLInputElement;

  if (key === 'Escape') {
    mode = SEARCH;
    txt.value = '';
    txt.blur();
    raf.scheduleFullRedraw();
    return;
  }
}

interface CmdAttrs {
  title: FuzzySegment[];
  subtitle: string;
  highlighted?: boolean;
  icon?: string;
  [htmlAttrs: string]: any;
}

class Cmd implements m.ClassComponent<CmdAttrs> {
  view({attrs}: m.Vnode<CmdAttrs>): void|m.Children {
    const {title, subtitle, icon, highlighted = false, ...htmlAttrs} = attrs;
    return m(
        'section.pf-cmd',
        {
          class: classNames(highlighted && 'pf-highlighted'),
          ...htmlAttrs,
        },
        m('h1', title.map(({value, matching}) => {
          return matching ? m('b', value) : value;
        })),
        m('h2', subtitle),
        m(Icon, {className: 'pf-right-icon', icon: icon ?? 'play_arrow'}),
    );
  }
}

class Omnibox implements m.ClassComponent {
  view() {
    const msgTTL = globals.state.status.timestamp + 1 - Date.now() / 1e3;
    const engineIsBusy =
        globals.state.engine !== undefined && !globals.state.engine.ready;

    if (msgTTL > 0 || engineIsBusy) {
      setTimeout(() => raf.scheduleFullRedraw(), msgTTL * 1000);
      return m(
          `.omnibox.message-mode`,
          m(`input[placeholder=${globals.state.status.msg}][readonly]`, {
            value: '',
          }));
    }
    return m(
        Popup,
        {
          isOpen: mode === COMMAND,
          trigger: this.renderOmnibox(),
          className: 'pf-popup-padded',
        },
        m(
            '.pf-cmd-container',
            this.renderCommandDropdown(),
            ),
    );
  }

  private renderCommandDropdown(): m.Children {
    if (mode === COMMAND) {
      const searchTerm = globals.state.omniboxState.omnibox;
      matchingCommands = globals.commandManager.fuzzyFilterCommands(searchTerm);
      if (matchingCommands.length === 0) {
        return m(EmptyState, {header: 'No matching commands'});
      } else {
        return matchingCommands.map((cmd, index) => {
          return m(Cmd, {
            title: cmd.segments,
            subtitle: cmd.id,
            highlighted: index === highlightedCommandIndex,
            onclick: () => {
              globals.commandManager.runCommand(cmd.id);
              mode = SEARCH;
              globals.dispatch(Actions.setOmnibox({
                omnibox: '',
                mode: 'SEARCH',
              }));
              highlightedCommandIndex = 0;
            },
          });
        });
      }
    } else {
      return null;
    }
  }

  private renderOmnibox() {
    const queryMode = mode === QUERY;
    const classes = classNames(
        mode === QUERY && 'query-mode',
        mode === COMMAND && 'command-mode',
    );
    return m(
        `.omnibox`,
        {
          class: classes,
        },
        m('input', {
          placeholder: PLACEHOLDER[mode],
          onkeydown: (e: Event) => onKeyDown(e),
          onkeyup: (e: Event) => onKeyUp(e),
          oninput: (e: InputEvent) => {
            const value = (e.target as HTMLInputElement).value;
            globals.dispatch(Actions.setOmnibox({
              omnibox: value,
              mode: queryMode ? 'COMMAND' : 'SEARCH',
            }));
            if (mode === SEARCH) {
              displayStepThrough = value.length > 0;
              globals.dispatch(Actions.setSearchIndex({index: -1}));
            }
          },
          value: globals.state.omniboxState.omnibox,
        }),
        displayStepThrough ?
            m('.stepthrough',
              m('.current',
                `${
                    globals.currentSearchResults.totalResults === 0 ?
                        '0 / 0' :
                        `${globals.state.searchIndex + 1} / ${
                            globals.currentSearchResults.totalResults}`}`),
              m('button',
                {
                  onclick: () => {
                    executeSearch(true /* reverse direction */);
                  },
                },
                m('i.material-icons.left', 'keyboard_arrow_left')),
              m('button',
                {
                  onclick: () => {
                    executeSearch();
                  },
                },
                m('i.material-icons.right', 'keyboard_arrow_right'))) :
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
    raf.addRedrawCallback(this.loading);
  }

  onremove() {
    raf.removeRedrawCallback(this.loading);
  }

  view() {
    return m('.progress');
  }

  loadingAnimation() {
    if (this.progressBar === undefined) return;
    const engine = globals.getCurrentEngine();
    if ((engine && !engine.ready) || globals.numQueuedQueries > 0 ||
        taskTracker.hasPendingTasks()) {
      this.progressBar.classList.add('progress-anim');
    } else {
      this.progressBar.classList.remove('progress-anim');
    }
  }
}


class NewVersionNotification implements m.ClassComponent {
  view() {
    return m(
        '.new-version-toast',
        `Updated to ${VERSION} and ready for offline use!`,
        m('button.notification-btn.preferred',
          {
            onclick: () => {
              globals.frontendLocalState.newVersionAvailable = false;
              raf.scheduleFullRedraw();
            },
          },
          'Dismiss'),
    );
  }
}


class HelpPanningNotification implements m.ClassComponent {
  view() {
    const dismissed = localStorage.getItem(DISMISSED_PANNING_HINT_KEY);
    // Do not show the help notification in embedded mode because local storage
    // does not persist for iFrames. The host is responsible for communicating
    // to users that they can press '?' for help.
    if (globals.embeddedMode || dismissed === 'true' ||
        !globals.frontendLocalState.showPanningHint) {
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
              raf.scheduleFullRedraw();
            },
          },
          'Dismiss'),
    );
  }
}

class TraceErrorIcon implements m.ClassComponent {
  view() {
    if (globals.embeddedMode) return;

    const errors = globals.traceErrors;
    if (!errors && !globals.metricError || mode === QUERY) return;
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
        {class: globals.state.sidebarVisible ? '' : 'hide-sidebar'},
        globals.frontendLocalState.newVersionAvailable ?
            m(NewVersionNotification) :
            m(Omnibox),
        m(Progress),
        m(HelpPanningNotification),
        m(TraceErrorIcon));
  }
}
