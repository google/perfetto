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
import {assertExists} from '../base/logging';
import {VERSION} from '../gen/perfetto_version';
import {StatusResult, TraceProcessorApiVersion} from '../protos';
import {HttpRpcEngine} from '../trace_processor/http_rpc_engine';
import {showModal} from '../widgets/modal';
import {AppImpl} from '../core/app_impl';

const CURRENT_API_VERSION =
  TraceProcessorApiVersion.TRACE_PROCESSOR_CURRENT_API_VERSION;

function getPromptMessage(tpStatus: StatusResult): string {
  return `Trace Processor Native Accelerator detected on ${HttpRpcEngine.hostAndPort} with:
${tpStatus.loadedTraceName}

YES, use loaded trace:
Will load from the current state of Trace Processor. If you did run
trace_processor_shell --httpd file.pftrace this is likely what you want.

YES, but reset state:
Use this if you want to open another trace but still use the
accelerator. This is the equivalent of killing and restarting
trace_processor_shell --httpd.

NO, Use builtin WASM:
Will not use the accelerator in this tab.

Using the native accelerator has some minor caveats:
- Only one tab can be using the accelerator.
- Sharing, downloading and conversion-to-legacy aren't supported.
`;
}

function getIncompatibleRpcMessage(tpStatus: StatusResult): string {
  return `The Trace Processor instance on ${HttpRpcEngine.hostAndPort} is too old.

This UI requires TraceProcessor features that are not present in the
Trace Processor native accelerator you are currently running.
If you continue, this is almost surely going to cause UI failures.

Please update your local Trace Processor binary:

curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
./trace_processor --httpd

UI version code: ${VERSION}
UI RPC API: ${CURRENT_API_VERSION}

Trace processor version: ${tpStatus.humanReadableVersion}
Trace processor version code: ${tpStatus.versionCode}
Trace processor RPC API: ${tpStatus.apiVersion}
`;
}

function getVersionMismatchMessage(tpStatus: StatusResult): string {
  return `The trace processor instance on ${HttpRpcEngine.hostAndPort} is a different build from the UI.

This may cause problems. Where possible it is better to use the matched version of the UI.
You can do this by clicking the button below.

UI version code: ${VERSION}
UI RPC API: ${CURRENT_API_VERSION}

Trace processor version: ${tpStatus.humanReadableVersion}
Trace processor version code: ${tpStatus.versionCode}
Trace processor RPC API: ${tpStatus.apiVersion}
`;
}

// The flow is fairly complicated:
// +-----------------------------------+
// |        User loads the UI          |
// +-----------------+-----------------+
//                   |
// +-----------------+-----------------+
// |   Is trace_processor present at   |
// |   HttpRpcEngine.hostAndPort?      |
// +--------------------------+--------+
//    |No                     |Yes
//    |        +--------------+-------------------------------+
//    |        |  Does version code of UI and TP match?       |
//    |        +--------------+----------------------------+--+
//    |                       |No                          |Yes
//    |                       |                            |
//    |                       |                            |
//    |         +-------------+-------------+              |
//    |         |Is a build of the UI at the|              |
//    |         |TP version code existant   |              |
//    |         |and reachable?             |              |
//    |         +---+----------------+------+              |
//    |             | No             | Yes                 |
//    |             |                |                     |
//    |             |       +--------+-------+             |
//    |             |       |Dialog: Mismatch|             |
//    |             |       |Load matched UI +-------------------------------+
//    |             |       |Continue        +-+           |                 |
//    |             |       +----------------+ |           |                 |
//    |             |                          |           |                 |
//    |      +------+--------------------------+----+      |                 |
//    |      |TP RPC version >= UI RPC version      |      |                 |
//    |      +----+-------------------+-------------+      |                 |
//    |           | No                |Yes                 |                 |
//    |      +----+--------------+    |                    |                 |
//    |      |Dialog: Bad RPC    |    |                    |                 |
//    |  +---+Use built-in WASM  |    |                    |                 |
//    |  |   |Continue anyway    +----|                    |                 |
//    |  |   +-------------------+    |        +-----------+-----------+     |
//    |  |                            +--------+TP has preloaded trace?|     |
//    |  |                                     +-+---------------+-----+     |
//    |  |                                       |No             |Yes        |
//    |  |                                       |  +---------------------+  |
//    |  |                                       |  | Dialog: Preloaded?  |  |
//    |  |                                       |  + YES, use loaded trace  |
//    |  |                                 +--------| YES, but reset state|  |
//    |  |  +---------------------------------------| NO, Use builtin Wasm|  |
//    |  |  |                              |     |  +---------------------+  |
//    |  |  |                              |     |                           |
//    |  |  |                           Reset TP |                           |
//    |  |  |                              |     |                           |
//    |  |  |                              |     |                           |
//  Show the UI                         Show the UI                  Link to
//  (WASM mode)                         (RPC mode)                   matched UI

// There are three options in the end:
// - Show the UI (WASM mode)
// - Show the UI (RPC mode)
// - Redirect to a matched version of the UI

// Try to connect to the external Trace Processor HTTP RPC accelerator (if
// available, often it isn't). If connected it will populate the
// |httpRpcState| in the frontend local state. In turn that will show the UI
// chip in the sidebar. trace_controller.ts will repeat this check before
// trying to load a new trace. We do this ahead of time just to have a
// consistent UX (i.e. so that the user can tell if the RPC is working without
// having to open a trace).
export async function CheckHttpRpcConnection(): Promise<void> {
  const state = await HttpRpcEngine.checkConnection();
  AppImpl.instance.httpRpc.httpRpcAvailable = state.connected;
  if (!state.connected) {
    // No RPC = exit immediately to the WASM UI.
    return;
  }
  const tpStatus = assertExists(state.status);

  function forceWasm() {
    AppImpl.instance.httpRpc.newEngineMode = 'FORCE_BUILTIN_WASM';
  }

  // Check short version:
  if (tpStatus.versionCode !== '' && tpStatus.versionCode !== VERSION) {
    const url = await isVersionAvailable(tpStatus.versionCode);
    if (url !== undefined) {
      // If matched UI available show a dialog asking the user to
      // switch.
      const result = await showDialogVersionMismatch(tpStatus, url);
      switch (result) {
        case MismatchedVersionDialog.Dismissed:
        case MismatchedVersionDialog.UseMatchingUi:
          navigateToVersion(tpStatus.versionCode);
          return;
        case MismatchedVersionDialog.UseMismatchedRpc:
          break;
        case MismatchedVersionDialog.UseWasm:
          forceWasm();
          return;
        default:
          const x: never = result;
          throw new Error(`Unsupported result ${x}`);
      }
    }
  }

  // Check the RPC version:
  if (tpStatus.apiVersion < CURRENT_API_VERSION) {
    const result = await showDialogIncompatibleRPC(tpStatus);
    switch (result) {
      case IncompatibleRpcDialogResult.Dismissed:
      case IncompatibleRpcDialogResult.UseWasm:
        forceWasm();
        return;
      case IncompatibleRpcDialogResult.UseIncompatibleRpc:
        break;
      default:
        const x: never = result;
        throw new Error(`Unsupported result ${x}`);
    }
  }

  // Check if pre-loaded:
  if (tpStatus.loadedTraceName) {
    // If a trace is already loaded in the trace processor (e.g., the user
    // launched trace_processor_shell -D trace_file.pftrace), prompt the user to
    // initialize the UI with the already-loaded trace.
    const result = await showDialogToUsePreloadedTrace(tpStatus);
    switch (result) {
      case PreloadedDialogResult.Dismissed:
      case PreloadedDialogResult.UseRpcWithPreloadedTrace:
        AppImpl.instance.openTraceFromHttpRpc();
        return;
      case PreloadedDialogResult.UseRpc:
        // Resetting state is the default.
        return;
      case PreloadedDialogResult.UseWasm:
        forceWasm();
        return;
      default:
        const x: never = result;
        throw new Error(`Unsupported result ${x}`);
    }
  }
}

enum MismatchedVersionDialog {
  UseMatchingUi = 'useMatchingUi',
  UseWasm = 'useWasm',
  UseMismatchedRpc = 'useMismatchedRpc',
  Dismissed = 'dismissed',
}

async function showDialogVersionMismatch(
  tpStatus: StatusResult,
  url: string,
): Promise<MismatchedVersionDialog> {
  let result = MismatchedVersionDialog.Dismissed;
  await showModal({
    title: 'Version mismatch',
    content: m('.modal-pre', getVersionMismatchMessage(tpStatus)),
    buttons: [
      {
        primary: true,
        text: `Open ${url}`,
        action: () => {
          result = MismatchedVersionDialog.UseMatchingUi;
        },
      },
      {
        text: 'Use builtin Wasm',
        action: () => {
          result = MismatchedVersionDialog.UseWasm;
        },
      },
      {
        text: 'Use mismatched version regardless (might crash)',
        action: () => {
          result = MismatchedVersionDialog.UseMismatchedRpc;
        },
      },
    ],
  });
  return result;
}

enum IncompatibleRpcDialogResult {
  UseWasm = 'useWasm',
  UseIncompatibleRpc = 'useIncompatibleRpc',
  Dismissed = 'dismissed',
}

async function showDialogIncompatibleRPC(
  tpStatus: StatusResult,
): Promise<IncompatibleRpcDialogResult> {
  let result = IncompatibleRpcDialogResult.Dismissed;
  await showModal({
    title: 'Incompatible RPC version',
    content: m('.modal-pre', getIncompatibleRpcMessage(tpStatus)),
    buttons: [
      {
        text: 'Use builtin Wasm',
        primary: true,
        action: () => {
          result = IncompatibleRpcDialogResult.UseWasm;
        },
      },
      {
        text: 'Use old version regardless (will crash)',
        action: () => {
          result = IncompatibleRpcDialogResult.UseIncompatibleRpc;
        },
      },
    ],
  });
  return result;
}

enum PreloadedDialogResult {
  UseRpcWithPreloadedTrace = 'useRpcWithPreloadedTrace',
  UseRpc = 'useRpc',
  UseWasm = 'useWasm',
  Dismissed = 'dismissed',
}

async function showDialogToUsePreloadedTrace(
  tpStatus: StatusResult,
): Promise<PreloadedDialogResult> {
  let result = PreloadedDialogResult.Dismissed;
  await showModal({
    title: 'Use trace processor native acceleration?',
    content: m('.modal-pre', getPromptMessage(tpStatus)),
    buttons: [
      {
        text: 'YES, use loaded trace',
        primary: true,
        action: () => {
          result = PreloadedDialogResult.UseRpcWithPreloadedTrace;
        },
      },
      {
        text: 'YES, but reset state',
        action: () => {
          result = PreloadedDialogResult.UseRpc;
        },
      },
      {
        text: 'NO, Use builtin WASM',
        action: () => {
          result = PreloadedDialogResult.UseWasm;
        },
      },
    ],
  });
  return result;
}

function getUrlForVersion(versionCode: string): string {
  const url = `${window.location.origin}/${versionCode}/`;
  return url;
}

async function isVersionAvailable(
  versionCode: string,
): Promise<string | undefined> {
  if (versionCode === '') {
    return undefined;
  }
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 1000);
  const url = getUrlForVersion(versionCode);
  let r;
  try {
    r = await fetch(url, {signal: controller.signal});
  } catch (e) {
    console.error(
      `No UI version for ${versionCode} at ${url}. This is an error if ${versionCode} is a released Perfetto version`,
    );
    return undefined;
  } finally {
    clearTimeout(timeoutId);
  }
  if (!r.ok) {
    return undefined;
  }
  return url;
}

function navigateToVersion(versionCode: string): void {
  const url = getUrlForVersion(versionCode);
  if (url === undefined) {
    throw new Error(`No URL known for UI version ${versionCode}.`);
  }
  window.location.replace(url);
}
