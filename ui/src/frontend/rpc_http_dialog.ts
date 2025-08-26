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
import protos from '../protos';
import {assertExists} from '../base/logging';
import {VERSION} from '../gen/perfetto_version';
import {HttpRpcEngine} from '../trace_processor/http_rpc_engine';
import {showModal, closeModal} from '../widgets/modal';
import {AppImpl} from '../core/app_impl';

const CURRENT_API_VERSION =
  protos.TraceProcessorApiVersion.TRACE_PROCESSOR_CURRENT_API_VERSION;

function getPromptMessage(tpStatus: protos.StatusResult): string {
  return `Trace Processor detected on ${HttpRpcEngine.hostAndPort} one or more loaded traces including 
  ${tpStatus.loadedTraceName}.

YES, select loaded trace:
Pops up a window that allows you to select the trace to load.
Will load from the current state of Trace Processor. If you did run
trace_processor_shell --httpd file.pftrace this is likely what you want.

YES, but reset state:
Use this if you want to open another trace but still use the
accelerator. This is the equivalent of killing and restarting
trace_processor_shell --httpd.

NO, Use builtin WASM:
Will not use the accelerator in this tab.

Using the native accelerator has some minor caveats:
- Sharing, downloading and conversion-to-legacy aren't supported.
`;
}

function getIncompatibleRpcMessage(tpStatus: protos.StatusResult): string {
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

function getVersionMismatchMessage(tpStatus: protos.StatusResult): string {
  return `The Trace Processor instance on ${HttpRpcEngine.hostAndPort} is a different build from the UI.

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
  const tpStatusAll = assertExists(state.status);

  // use the first trace processor if available, otherwise fallback
  const firstStatusData = tpStatusAll.traceProcessorStatuses?.[0]?.status;
  if (!firstStatusData) {
    // No trace processors available, use RPC without preloaded trace
    return;
  }

  // Create a proper StatusResult instance from the IStatusResult interface
  const firstStatus = protos.StatusResult.create(firstStatusData);

  function forceWasm() {
    AppImpl.instance.httpRpc.newEngineMode = 'FORCE_BUILTIN_WASM';
  }

  // Check short version:
  if (firstStatus.versionCode !== '' && firstStatus.versionCode !== VERSION) {
    const url = await isVersionAvailable(firstStatus.versionCode);
    if (url !== undefined) {
      // If matched UI available show a dialog asking the user to
      // switch.
      const result = await showDialogVersionMismatch(firstStatus, url);
      switch (result) {
        case MismatchedVersionDialog.Dismissed:
        case MismatchedVersionDialog.UseMatchingUi:
          navigateToVersion(firstStatus.versionCode);
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
  if (firstStatus.apiVersion < CURRENT_API_VERSION) {
    const result = await showDialogIncompatibleRPC(firstStatus);
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
  if (firstStatus.loadedTraceName) {
    // If a trace is already loaded in the trace processor (e.g., the user
    // launched trace_processor_shell -D trace_file.pftrace), prompt the user to
    // initialize the UI with the already-loaded trace.
    const result = await showDialogToUsePreloadedTrace(firstStatus);
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
  tpStatus: protos.StatusResult,
  url: string,
): Promise<MismatchedVersionDialog> {
  let result = MismatchedVersionDialog.Dismissed;
  await showModal({
    title: 'Version mismatch',
    content: m('.pf-modal-pre', getVersionMismatchMessage(tpStatus)),
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
  tpStatus: protos.StatusResult,
): Promise<IncompatibleRpcDialogResult> {
  let result = IncompatibleRpcDialogResult.Dismissed;
  await showModal({
    title: 'Incompatible RPC version',
    content: m('.pf-modal-pre', getIncompatibleRpcMessage(tpStatus)),
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
  tpStatus: protos.StatusResult,
): Promise<PreloadedDialogResult> {
  const result = await new Promise<PreloadedDialogResult>((resolve) => {
    showModal({
      title: 'Use trace processor native acceleration?',
      content: m('.pf-modal-pre', getPromptMessage(tpStatus)),
      buttons: [
        {
          text: 'YES, select loaded trace',
          primary: true,
          action: () => {
            resolve(PreloadedDialogResult.UseRpcWithPreloadedTrace);
          },
        },
        {
          text: 'YES, but reset state',
          action: () => {
            resolve(PreloadedDialogResult.UseRpc);
          },
        },
        {
          text: 'NO, Use builtin WASM',
          action: () => {
            resolve(PreloadedDialogResult.UseWasm);
          },
        },
      ],
    });
  });

  // If user selected "YES, select loaded trace", show trace processor selection
  if (result === PreloadedDialogResult.UseRpcWithPreloadedTrace) {
    const selectedUuid = await showTraceProcessorSelectionModal();

    if (selectedUuid !== null && selectedUuid !== undefined) {
      console.log(`Selected trace processor: ${selectedUuid}`);
      // Store the selected UUID for backend integration
      AppImpl.instance.httpRpc.selectedTraceProcessorUuid = selectedUuid;
      return PreloadedDialogResult.UseRpcWithPreloadedTrace;
    } else {
      // User cancelled the selection
      return PreloadedDialogResult.Dismissed;
    }
  }

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
      `No UI version for ${versionCode} at ${url}. ` +
        `This is an error if ${versionCode} is a released Perfetto version`,
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

// New function to show trace processor selection modal
async function showTraceProcessorSelectionModal(): Promise<string | undefined> {
  return new Promise(async (resolve) => {
    try {
      // Fetch actual trace processor data
      const httpRpcState = await HttpRpcEngine.checkConnection();

      if (!httpRpcState.connected || !httpRpcState.status) {
        showModal({
          title: 'No Trace Processors Available',
          content:
            'Could not connect to any trace processors or no trace processors are available.',
          buttons: [
            {
              text: 'OK',
              primary: true,
              action: () => resolve(undefined),
            },
          ],
        });
        return;
      }

      const traceProcessors = httpRpcState.status.traceProcessorStatuses;

      if (traceProcessors.length === 0) {
        showModal({
          title: 'No Trace Processors Available',
          content: 'No trace processors are currently running.',
          buttons: [
            {
              text: 'OK',
              primary: true,
              action: () => resolve(undefined),
            },
          ],
        });
        return;
      }

      // Filter to only show trace processors that have a loaded trace
      const processorsWithTraces = traceProcessors.filter(
        (tp) => tp.status?.loadedTraceName && tp.status.loadedTraceName !== '',
      );

      if (processorsWithTraces.length === 0) {
        showModal({
          title: 'No Loaded Traces',
          content: 'No trace processors have loaded traces available.',
          buttons: [
            {
              text: 'OK',
              primary: true,
              action: () => resolve(undefined),
            },
          ],
        });
        return;
      }

      showModal({
        title: 'Select Trace Processor',
        content: () => {
          return m(
            '.tp-selection-modal',
            m('p', 'Please select a trace processor to use:'),
            m(
              '.tp-list',
              {
                style: {
                  'max-height': '300px',
                  'overflow-y': 'auto',
                  'border': '1px solid #ccc',
                  'border-radius': '4px',
                  'margin': '16px 0',
                },
              },
              processorsWithTraces.map((tp) => {
                const status = tp.status!;
                return m(
                  '.tp-item',
                  {
                    key: tp.uuid || 'default',
                    style: {
                      'padding': '12px',
                      'border-bottom': '1px solid #eee',
                      'cursor': 'pointer',
                      'display': 'flex',
                      'justify-content': 'space-between',
                      'align-items': 'center',
                    },
                    onclick: () => {
                      closeModal();
                      resolve(tp.uuid || '');
                    },
                    onmouseenter: (e: Event) => {
                      (e.target as HTMLElement).style.backgroundColor =
                        '#f5f5f5';
                    },
                    onmouseleave: (e: Event) => {
                      (e.target as HTMLElement).style.backgroundColor = '';
                    },
                  },
                  [
                    m('.tp-info', [
                      m('strong', status.loadedTraceName),
                      m('br'),
                      m('small', `UUID: ${tp.uuid || 'default'}`),
                    ]),
                    status.humanReadableVersion &&
                      m(
                        '.tp-version',
                        {
                          style: {
                            'font-size': '11px',
                            'padding': '2px 6px',
                            'border-radius': '8px',
                            'background-color': '#e6f3ff',
                            'color': '#1976d2',
                          },
                        },
                        status.humanReadableVersion,
                      ),
                  ],
                );
              }),
            ),
          );
        },
        buttons: [
          {
            text: 'Cancel',
            action: () => {
              closeModal();
              resolve(undefined);
            },
          },
        ],
      });
    } catch (error) {
      console.error('Error fetching trace processors:', error);
      showModal({
        title: 'Error',
        content: 'Failed to fetch trace processor information.',
        buttons: [
          {
            text: 'OK',
            primary: true,
            action: () => resolve(undefined),
          },
        ],
      });
    }
  });
}
