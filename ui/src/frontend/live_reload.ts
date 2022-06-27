// Copyright (C) 2021 The Android Open Source Project
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

import {featureFlags} from '../common/feature_flags';
import {globals} from './globals';

let lastReloadDialogTime = 0;
const kMinTimeBetweenDialogsMs = 10000;
const changedPaths = new Set<string>();

export function initLiveReloadIfLocalhost() {
  if (!location.origin.startsWith('http://localhost:')) return;
  if (globals.embeddedMode) return;

  const monitor = new EventSource('/live_reload');
  monitor.onmessage = (msg) => {
    const change = msg.data;
    console.log('Live reload:', change);
    changedPaths.add(change);
    if (change.endsWith('.css')) {
      reloadCSS();
    } else if (change.endsWith('.html') || change.endsWith('.js')) {
      reloadDelayed();
    }
  };
  monitor.onerror = (err) => {
    // In most cases the error is fired on reload, when the socket disconnects.
    // Delay the error and the reconnection, so in the case of a reload we don't
    // see any midleading message.
    setTimeout(() => console.error('LiveReload SSE error', err), 1000);
  };
}

function reloadCSS() {
  const css = document.querySelector('link[rel=stylesheet]') as HTMLLinkElement;
  if (!css) return;
  const parent = css.parentElement!;
  parent.removeChild(css);
  parent.appendChild(css);
}

const rapidReloadFlag = featureFlags.register({
  id: 'rapidReload',
  name: 'Development: rapid live reload',
  defaultValue: false,
  description: 'During development, instantly reload the page on change. ' +
      'Enables lower latency of live reload at the cost of potential ' +
      'multiple re-reloads.',
  devOnly: true,
});

function reloadDelayed() {
  setTimeout(() => {
    let pathsStr = '';
    for (const path of changedPaths) {
      pathsStr += path + '\n';
    }
    changedPaths.clear();
    if (Date.now() - lastReloadDialogTime < kMinTimeBetweenDialogsMs) return;
    const reload =
        rapidReloadFlag.get() || confirm(`${pathsStr}changed, click to reload`);
    lastReloadDialogTime = Date.now();
    if (reload) {
      window.location.reload();
    }
  }, rapidReloadFlag.get() ? 0 : 1000);
}
