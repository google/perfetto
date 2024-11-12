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
import {taskTracker} from './task_tracker';
import {Popup, PopupPosition} from '../widgets/popup';
import {assertFalse} from '../base/logging';
import {OmniboxMode} from '../core/omnibox_manager';
import {AppImpl} from '../core/app_impl';
import {TraceImpl, TraceImplAttrs} from '../core/trace_impl';

class Progress implements m.ClassComponent<TraceImplAttrs> {
  view({attrs}: m.CVnode<TraceImplAttrs>): m.Children {
    const engine = attrs.trace.engine;
    const isLoading =
      AppImpl.instance.isLoadingTrace ||
      engine.numRequestsPending > 0 ||
      taskTracker.hasPendingTasks();
    const classes = classNames(isLoading && 'progress-anim');
    return m('.progress', {class: classes});
  }
}

class TraceErrorIcon implements m.ClassComponent<TraceImplAttrs> {
  private tracePopupErrorDismissed = false;

  view({attrs}: m.CVnode<TraceImplAttrs>) {
    const trace = attrs.trace;
    if (AppImpl.instance.embeddedMode) return;

    const mode = AppImpl.instance.omnibox.mode;
    const totErrors = trace.traceInfo.importErrors + trace.loadingErrors.length;
    if (totErrors === 0 || mode === OmniboxMode.Command) {
      return;
    }
    const message = Boolean(totErrors)
      ? `${totErrors} import or data loss errors detected.`
      : `Metric error detected.`;
    return m(
      '.error-box',
      m(
        Popup,
        {
          trigger: m('.popup-trigger'),
          isOpen: !this.tracePopupErrorDismissed,
          position: PopupPosition.Left,
          onChange: (shouldOpen: boolean) => {
            assertFalse(shouldOpen);
            this.tracePopupErrorDismissed = true;
          },
        },
        m('.error-popup', 'Data-loss/import error. Click for more info.'),
      ),
      m(
        'a.error',
        {href: '#!/info'},
        m(
          'i.material-icons',
          {
            title: message + ` Click for more info.`,
          },
          'announcement',
        ),
      ),
    );
  }
}

export interface TopbarAttrs {
  omnibox: m.Children;
  trace?: TraceImpl;
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  view({attrs}: m.Vnode<TopbarAttrs>) {
    const {omnibox} = attrs;
    return m(
      '.topbar',
      {
        class: AppImpl.instance.sidebar.visible ? '' : 'hide-sidebar',
      },
      omnibox,
      attrs.trace && m(Progress, {trace: attrs.trace}),
      attrs.trace && m(TraceErrorIcon, {trace: attrs.trace}),
    );
  }
}
