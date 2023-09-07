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

import {classNames} from '../base/classnames';
import {raf} from '../core/raf_scheduler';
import {VERSION} from '../gen/perfetto_version';

import {globals} from './globals';
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
  omnibox: m.Children;
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  view({attrs}: m.Vnode<TopbarAttrs>) {
    const {omnibox} = attrs;
    return m(
        '.topbar',
        {class: globals.state.sidebarVisible ? '' : 'hide-sidebar'},
        globals.frontendLocalState.newVersionAvailable ?
            m(NewVersionNotification) :
            omnibox,
        m(Progress),
        m(HelpPanningNotification),
        m(TraceErrorIcon));
  }
}
