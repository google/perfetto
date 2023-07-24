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

import {findRef} from '../base/dom_utils';
import {Actions} from '../common/actions';
import {raf} from '../core/raf_scheduler';
import {VERSION} from '../gen/perfetto_version';

import {classNames} from './classnames';
import {globals} from './globals';
import {Omnibox, OmniboxOption} from './omnibox';
import {runQueryInNewTab} from './query_result_tab';
import {executeSearch} from './search_handler';
import {taskTracker} from './task_tracker';

export const DISMISSED_PANNING_HINT_KEY = 'dismissedPanningHint';

class Progress implements m.ClassComponent {
  view(_vnode: m.Vnode): m.Children {
    const classes = classNames(this.isLoading() && 'progress-anim');
    return m('.progress', {class: classes});
  }

  private isLoading(): boolean {
    const engine = globals.getCurrentEngine();
    return (
        (engine && !engine.ready) || globals.numQueuedQueries > 0 ||
        taskTracker.hasPendingTasks());
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

    const mode = globals.state.omniboxState.mode;

    const errors = globals.traceErrors;
    if (!errors && !globals.metricError || mode === 'COMMAND') return;
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

export interface TopbarAttrs {
  commandMode: boolean;
  commandText: string;
  onCommandModeChange?: (commandMode: boolean) => void;
  onCommandValueChange?: (value: string) => void;
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  private omniboxQueryValue = '';
  private omniboxInputEl?: HTMLInputElement;

  static readonly OMNIBOX_INPUT_REF = 'omnibox';

  view({attrs}: m.Vnode<TopbarAttrs>) {
    return m(
        '.topbar',
        {class: globals.state.sidebarVisible ? '' : 'hide-sidebar'},
        globals.frontendLocalState.newVersionAvailable ?
            m(NewVersionNotification) :
            this.renderOmnibox(attrs),
        m(Progress),
        m(HelpPanningNotification),
        m(TraceErrorIcon));
  }

  renderOmnibox(attrs: TopbarAttrs): m.Children {
    const msgTTL = globals.state.status.timestamp + 1 - Date.now() / 1e3;
    const engineIsBusy =
        globals.state.engine !== undefined && !globals.state.engine.ready;

    if (msgTTL > 0 || engineIsBusy) {
      setTimeout(() => raf.scheduleFullRedraw(), msgTTL * 1000);
      return m(
          `.omnibox.message-mode`,
          m(`input[placeholder=${
                globals.state.status.msg}][readonly][disabled][ref=omnibox]`,
            {
              value: '',
            }));
    }

    const {commandMode} = attrs;
    if (commandMode) {
      return this.renderCommandOmnibox(attrs);
    } else {
      const mode = globals.state.omniboxState.mode;
      switch (mode) {
        case 'COMMAND':
          // COMMAND is the previous term for query - let's avoid changing this.
          return this.renderQueryOmnibox();
        case 'SEARCH':
          return this.renderSearchOmnibox(attrs);
        default:
          const x: never = mode;
          throw new Error(`Unhandled omnibox mode ${x}`);
      }
    }
  }

  renderSearchOmnibox(attrs: TopbarAttrs): m.Children {
    const {
      onCommandModeChange = () => {},
    } = attrs;

    const omniboxState = globals.state.omniboxState;
    const displayStepThrough =
        omniboxState.omnibox.length >= 4 || omniboxState.force;

    return m(Omnibox, {
      value: globals.state.omniboxState.omnibox,
      placeholder: 'Search...',
      inputRef: Topbar.OMNIBOX_INPUT_REF,
      onInput: (value, prev) => {
        if (prev === '') {
          if (value === '>') {
            onCommandModeChange(true);
            return;
          } else if (value === ':') {
            // Switch to query mode.
            globals.dispatch(Actions.setOmniboxMode({mode: 'COMMAND'}));
            return;
          }
        }
        globals.dispatch(Actions.setOmnibox({omnibox: value, mode: 'SEARCH'}));
      },
      onClose: () => {
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (value, _mod, shift) => {
        executeSearch(shift);
        globals.dispatch(
            Actions.setOmnibox({omnibox: value, mode: 'SEARCH', force: true}));
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      rightContent: displayStepThrough && this.renderStepThrough(),
    });
  }

  private renderStepThrough() {
    return m(
        '.stepthrough',
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
          m('i.material-icons.right', 'keyboard_arrow_right')));
  }

  renderCommandOmnibox(attrs: TopbarAttrs): m.Children {
    const {
      onCommandModeChange = () => {},
      commandText,
      onCommandValueChange = () => {},
    } = attrs;

    const cmdMgr = globals.commandManager;
    const filteredCmds = cmdMgr.fuzzyFilterCommands(commandText);
    const options: OmniboxOption[] = filteredCmds.map(({segments, id}) => {
      return {
        key: id,
        displayName: segments,
      };
    });

    return m(Omnibox, {
      value: commandText,
      placeholder: 'Start typing a command...',
      inputRef: Topbar.OMNIBOX_INPUT_REF,
      extraClasses: 'command-mode',
      options,
      closeOnSubmit: true,
      closeOnOutsideClick: true,
      onInput: (value) => {
        onCommandValueChange(value);
      },
      onClose: () => {
        onCommandModeChange(false);
        onCommandValueChange('');
        globals.dispatch(Actions.setOmniboxMode({mode: 'SEARCH'}));
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
      onSubmit: (key: string) => {
        cmdMgr.runCommand(key);
      },
    });
  }

  renderQueryOmnibox(): m.Children {
    const ph = 'e.g. select * from sched left join thread using(utid) limit 10';
    return m(Omnibox, {
      value: this.omniboxQueryValue,
      placeholder: ph,
      inputRef: Topbar.OMNIBOX_INPUT_REF,
      extraClasses: 'query-mode',
      onInput: (value) => {
        this.omniboxQueryValue = value;
        raf.scheduleFullRedraw();
      },
      onSubmit: (value, alt) => {
        runQueryInNewTab(
            value,
            alt ? 'Pinned query' : 'Omnibox query',
            alt ? undefined : 'omnibox_query');
      },
      onClose: () => {
        this.omniboxQueryValue = '';
        globals.dispatch(Actions.setOmniboxMode({mode: 'SEARCH'}));
        if (this.omniboxInputEl) {
          this.omniboxInputEl.blur();
        }
      },
    });
  }

  oncreate({dom}: m.VnodeDOM<TopbarAttrs, this>) {
    const el = findRef(dom, Topbar.OMNIBOX_INPUT_REF);
    if (el && el instanceof HTMLInputElement) {
      this.omniboxInputEl = el;
    }
  }

  onupdate({dom}: m.VnodeDOM<TopbarAttrs, this>) {
    const el = findRef(dom, Topbar.OMNIBOX_INPUT_REF);
    if (el && el instanceof HTMLInputElement) {
      this.omniboxInputEl = el;
    }
  }
}
