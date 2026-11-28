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

import {binaryDecode} from '../base/string_utils';
import {ChromeTracingController} from './chrome_tracing_controller';

enableOnlyOnPerfettoHost();

// Listen for messages from the perfetto ui.
// eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
if (globalThis.chrome) {
  chrome.runtime.onConnectExternal.addListener((port) => {
    // Bind one controller to this connection only: every externally
    // connectable page gets its own controller and its own dispatcher, so a
    // page can never take over or observe another page's tracing session.
    const controller = new ChromeTracingController(port);
    port.onMessage.addListener((message) =>
      onUIMessage(message, port, controller),
    );
    port.onDisconnect.addListener(() => controller.dispose());
  });
}

function onUIMessage(
  message: {method: string; requestData: string},
  port: chrome.runtime.Port,
  controller: ChromeTracingController,
) {
  // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
  if (!message || typeof message !== 'object') {
    return;
  }
  if (message.method === 'ExtensionVersion') {
    port.postMessage({version: chrome.runtime.getManifest().version});
    return;
  }
  // ChromeExtensionConsumerPort sends the request data as string because
  // chrome.runtime.port doesn't support ArrayBuffers.
  const requestDataArray: Uint8Array = message.requestData
    ? binaryDecode(message.requestData)
    : new Uint8Array();
  controller.handleCommand(message.method, requestDataArray);
}

function enableOnlyOnPerfettoHost() {
  function enableOnHostWithSuffix(suffix: string) {
    return {
      conditions: [
        new chrome.declarativeContent.PageStateMatcher({
          pageUrl: {hostSuffix: suffix},
        }),
      ],
      actions: [new chrome.declarativeContent.ShowPageAction()],
    };
  }
  chrome.declarativeContent.onPageChanged.removeRules(undefined, () => {
    chrome.declarativeContent.onPageChanged.addRules([
      enableOnHostWithSuffix('localhost'),
      enableOnHostWithSuffix('127.0.0.1'),
      enableOnHostWithSuffix('.perfetto.dev'),
      enableOnHostWithSuffix('.storage.googleapis.com'),
    ]);
  });
}
