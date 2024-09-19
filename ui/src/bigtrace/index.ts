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

// Keep this import first.
import '../base/static_initializers';
import m from 'mithril';
import {defer} from '../base/deferred';
import {reportError, addErrorHandler, ErrorDetails} from '../base/logging';
import {initLiveReloadIfLocalhost} from '../core/live_reload';
import {raf} from '../core/raf_scheduler';
import {setScheduleFullRedraw} from '../widgets/raf';

function getRoot() {
  // Works out the root directory where the content should be served from
  // e.g. `http://origin/v1.2.3/`.
  const script = document.currentScript as HTMLScriptElement;

  // Needed for DOM tests, that do not have script element.
  if (script === null) {
    return '';
  }

  let root = script.src;
  root = root.substr(0, root.lastIndexOf('/') + 1);
  return root;
}

function setupContentSecurityPolicy() {
  // Note: self and sha-xxx must be quoted, urls data: and blob: must not.
  const policy = {
    'default-src': [
      `'self'`,
    ],
    'script-src': [
      `'self'`,
    ],
    'object-src': ['none'],
    'connect-src': [
      `'self'`,
    ],
    'img-src': [
      `'self'`,
      'data:',
      'blob:',
    ],
    'style-src': [
      `'self'`,
    ],
    'navigate-to': ['https://*.perfetto.dev', 'self'],
  };
  const meta = document.createElement('meta');
  meta.httpEquiv = 'Content-Security-Policy';
  let policyStr = '';
  for (const [key, list] of Object.entries(policy)) {
    policyStr += `${key} ${list.join(' ')}; `;
  }
  meta.content = policyStr;
  document.head.appendChild(meta);
}

function main() {
  // Wire up raf for widgets.
  setScheduleFullRedraw(() => raf.scheduleFullRedraw());

  setupContentSecurityPolicy();

  // Load the css. The load is asynchronous and the CSS is not ready by the time
  // appendChild returns.
  const root = getRoot();
  const cssLoadPromise = defer<void>();
  const css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = root + 'perfetto.css';
  css.onload = () => cssLoadPromise.resolve();
  css.onerror = (err) => cssLoadPromise.reject(err);
  const favicon = document.head.querySelector('#favicon') as HTMLLinkElement;
  if (favicon) favicon.href = root + 'assets/favicon.png';

  document.head.append(css);

  // Add Error handlers for JS error and for uncaught exceptions in promises.
  addErrorHandler((err: ErrorDetails) => console.log(err.message, err.stack));
  window.addEventListener('error', (e) => reportError(e));
  window.addEventListener('unhandledrejection', (e) => reportError(e));

  // Prevent pinch zoom.
  document.body.addEventListener('wheel', (e: MouseEvent) => {
    if (e.ctrlKey) e.preventDefault();
  }, {passive: false});

  cssLoadPromise.then(() => onCssLoaded());
}

function onCssLoaded() {
  // Clear all the contents of the initial page (e.g. the <pre> error message)
  // And replace it with the root <main> element which will be used by mithril.
  document.body.innerHTML = '';

  raf.domRedraw = () => {
    m.render(document.body, m('div'));
  };

  initLiveReloadIfLocalhost(false);
}

main();
