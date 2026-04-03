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
import {assertFalse} from '../base/assert';
import {AppImpl} from '../core/app_impl';
import {OmniboxMode} from '../core/omnibox_manager';
import {Router} from '../core/router';
import {TraceImpl, TraceImplAttrs} from '../core/trace_impl';
import {SidePanelManagerImpl} from '../core/side_panel_manager';
import {Button} from '../widgets/button';
import {Intent} from '../widgets/common';
import {Popup, PopupPosition} from '../widgets/popup';
import {Omnibox} from './omnibox';

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
      '.pf-topbar__error-box',
      m(
        Popup,
        {
          trigger: m('span'),
          isOpen: !this.tracePopupErrorDismissed,
          position: PopupPosition.Left,
          onChange: (shouldOpen) => {
            assertFalse(shouldOpen);
            this.tracePopupErrorDismissed = true;
          },
        },
        m(
          '.pf-topbar__error-popup',
          'Data-loss/import error. Click for more info.',
        ),
      ),
      m(Button, {
        icon: 'announcement',
        title: message + ` Click for more info.`,
        intent: Intent.Danger,
        onclick: () => {
          // Navigate to the info page when the button is clicked.
          Router.navigate('#!/info');
        },
      }),
    );
  }
}

export interface TopbarAttrs {
  readonly trace?: TraceImpl;
}

function renderSidePanelToggle(mgr: SidePanelManagerImpl): m.Children {
  const hasTabs = mgr.registeredTabs.length > 0;
  if (!hasTabs) return undefined;
  return m(Button, {
    icon: 'dock_to_left',
    title: 'Toggle sidebar',
    active: mgr.visible,
    iconFilled: mgr.visible,
    className: classNames('pf-topbar__side-panel-btn'),
    onclick: () => {
      if (mgr.visible) {
        mgr.visible = false;
      } else {
        // If no tab is open, show the first registered one.
        if (!mgr.currentTabUri && mgr.registeredTabs.length > 0) {
          mgr.showTab(mgr.registeredTabs[0].uri);
        } else {
          mgr.visible = true;
        }
      }
    },
  });
}

export class Topbar implements m.ClassComponent<TopbarAttrs> {
  view({attrs}: m.Vnode<TopbarAttrs>) {
    const {trace} = attrs;
    const app = AppImpl.instance;
    return m(
      '.pf-topbar',
      {
        className: classNames(
          !app.sidebar.visible && 'pf-topbar--hide-sidebar',
        ),
      },
      m(Omnibox, {trace}),
      m(
        '.pf-topbar__right',
        trace && m(TraceErrorIcon, {trace}),
        renderSidePanelToggle(app.sidePanel),
      ),
    );
  }
}
