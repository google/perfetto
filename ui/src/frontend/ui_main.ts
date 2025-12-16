// Copyright (C) 2023 The Android Open Source Project
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
import {AppImpl} from '../core/app_impl';
import {CookieConsent} from '../core/cookie_consent';
import {featureFlags} from '../core/feature_flags';
import {LinearProgress} from '../widgets/linear_progress';
import {maybeRenderFullscreenModalDialog} from '../widgets/modal';
import {initCssConstants} from './css_constants';
import {Sidebar} from './sidebar';
import {renderStatusBar} from './statusbar';
import {taskTracker} from './task_tracker';
import {Topbar} from './topbar';

const showStatusBarFlag = featureFlags.register({
  id: 'Enable status bar',
  description: 'Enable status bar at the bottom of the window',
  defaultValue: true,
});
const APP_TITLE = 'Perfetto UI';

// This components gets destroyed and recreated every time the current trace
// changes. Note that in the beginning the current trace is undefined.
export class UiMain implements m.ClassComponent {
  // This function is invoked once per trace.
  constructor() {
    const app = AppImpl.instance;
    const trace = app.trace;

    // Update the title bar to reflect the loaded trace's title
    if (trace) {
      document.title = `${trace.traceInfo.traceTitle || 'Trace'} - ${APP_TITLE}`;
    } else {
      document.title = APP_TITLE;
    }
  }

  view(): m.Children {
    // Update the trace reference on each render so that it's kept up to date.
    const app = AppImpl.instance;
    const trace = app.trace;

    const isSomethingLoading =
      app.isLoadingTrace ||
      (trace?.engine.numRequestsPending ?? 0) > 0 ||
      taskTracker.hasPendingTasks();

    return m('main.pf-ui-main', [
      m(Sidebar),
      m(Topbar, {trace}),
      m(LinearProgress, {
        className: 'pf-ui-main__loading',
        state: isSomethingLoading ? 'indeterminate' : 'none',
      }),
      m('.pf-ui-main__page-container', app.pages.renderPageForCurrentRoute()),
      m(CookieConsent),
      maybeRenderFullscreenModalDialog(),
      showStatusBarFlag.get() && renderStatusBar(trace),
      app.perfDebugging.renderPerfStats(),
    ]);
  }

  oncreate({dom}: m.VnodeDOM) {
    initCssConstants(dom);
  }
}
