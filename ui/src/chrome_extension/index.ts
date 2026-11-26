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

// externally_connectable match patterns can't restrict the port, so the
// localhost and 127.0.0.1 entries in the manifest let any page served from any
// local port connect and record a browser-wide trace via chrome.debugger.
// Check the sender here too. Localhost is only trusted when the extension is
// loaded unpacked, i.e. when someone is working on the UI locally.
const PERFETTO_UI_ORIGIN = 'https://ui.perfetto.dev';

let chromeTraceController: ChromeTracingController | undefined = undefined;

enableOnlyOnPerfettoHost();

// Listen for messages from the perfetto ui.
// eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
if (globalThis.chrome) {
  chrome.runtime.onConnectExternal.addListener((port) => {
    if (!isAllowedSender(port.sender)) {
      port.disconnect();
      return;
    }
    chromeTraceController = new ChromeTracingController(port);
    port.onMessage.addListener(onUIMessage);
  });
}

function isAllowedSender(sender?: chrome.runtime.MessageSender): boolean {
  const origin = sender?.origin;
  if (origin === undefined) return false;
  if (origin === PERFETTO_UI_ORIGIN) return true;

  // Store installs get an update_url added to their manifest, unpacked ones
  // don't.
  const isUnpacked = chrome.runtime.getManifest().update_url === undefined;
  if (!isUnpacked) return false;
  let hostname: string;
  try {
    hostname = new URL(origin).hostname;
  } catch {
    return false;
  }
  return hostname === 'localhost' || hostname === '127.0.0.1';
}

function onUIMessage(
  message: {method: string; requestData: string},
  port: chrome.runtime.Port,
) {
  if (message.method === 'ExtensionVersion') {
    port.postMessage({version: chrome.runtime.getManifest().version});
    return;
  }
  console.assert(chromeTraceController !== undefined);
  if (!chromeTraceController) return;
  // ChromeExtensionConsumerPort sends the request data as string because
  // chrome.runtime.port doesn't support ArrayBuffers.
  const requestDataArray: Uint8Array = message.requestData
    ? binaryDecode(message.requestData)
    : new Uint8Array();
  chromeTraceController.handleCommand(message.method, requestDataArray);
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
