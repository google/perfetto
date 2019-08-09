// Copyright (C) 2019 The Android Open Source Project
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

import {ChromeTracingController} from './chrome_tracing_controller';

let chromeTraceController: ChromeTracingController|undefined = undefined;

chrome.runtime.onInstalled.addListener(() => {
  enableOnlyOnPerfettoHost();
  // Listen for messages from the perfetto ui.
  chrome.runtime.onConnectExternal.addListener(port => {
    chromeTraceController = new ChromeTracingController(port);
    port.onMessage.addListener(onUIMessage);
  });
});

function onUIMessage(
    message: {method: string, traceConfig: Uint8Array},
    port: chrome.runtime.Port) {
  if (message.method === 'ExtensionVersion') {
    port.postMessage({version: chrome.runtime.getManifest().version});
    return;
  }
  // In the future, more targets will be supported.
  if (chromeTraceController) chromeTraceController.onMessage(message);
}

function enableOnlyOnPerfettoHost() {
  function enableOnHostWithSuffix(suffix: string) {
    return {
      conditions: [new chrome.declarativeContent.PageStateMatcher({
        pageUrl: {hostSuffix: suffix},
      })],
      actions: [new chrome.declarativeContent.ShowPageAction()]
    };
  }
  chrome.declarativeContent.onPageChanged.removeRules(undefined, () => {
    chrome.declarativeContent.onPageChanged.addRules([
      enableOnHostWithSuffix('.perfetto.local'),
      enableOnHostWithSuffix('.perfetto.dev'),
      enableOnHostWithSuffix('-dot-perfetto-ui.appspot.com'),
    ]);
  });
}
