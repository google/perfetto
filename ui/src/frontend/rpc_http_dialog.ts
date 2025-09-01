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
  const firstTpStatusData = tpStatusAll.traceProcessorStatuses?.[0]?.status;
  if (!firstTpStatusData) {
    // No trace processors available, use RPC without preloaded trace
    return;
  }

  // Create a proper StatusResult instance from the IStatusResult interface
  const firstTpStatus = protos.StatusResult.create(firstTpStatusData);

  function forceWasm() {
    AppImpl.instance.httpRpc.newEngineMode = 'FORCE_BUILTIN_WASM';
  }

  // Check short version:
  if (
    firstTpStatus.versionCode !== '' &&
    firstTpStatus.versionCode !== VERSION
  ) {
    const url = await isVersionAvailable(firstTpStatus.versionCode);
    if (url !== undefined) {
      // If matched UI available show a dialog asking the user to
      // switch.
      const result = await showDialogVersionMismatch(firstTpStatus, url);
      switch (result) {
        case MismatchedVersionDialog.Dismissed:
        case MismatchedVersionDialog.UseMatchingUi:
          navigateToVersion(firstTpStatus.versionCode);
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
  if (firstTpStatus.apiVersion < CURRENT_API_VERSION) {
    const result = await showDialogIncompatibleRPC(firstTpStatus);
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
  if (firstTpStatus.loadedTraceName) {
    // If a trace is already loaded in the trace processor (e.g., the user
    // launched trace_processor_shell -D trace_file.pftrace), prompt the user to
    // initialize the UI with the already-loaded trace.
    const result = await showDialogToUsePreloadedTrace(firstTpStatus);
    switch (result) {
      case PreloadedDialogResult.Dismissed:
        closeModal();
        return;
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
  return new Promise<PreloadedDialogResult>(async (resolve) => {
    try {
      // Fetch actual trace processor data for the comprehensive modal
      const httpRpcState = await HttpRpcEngine.checkConnection();

      let traceProcessors: Array<protos.ITraceProcessorStatus> = [];
      if (httpRpcState.connected && httpRpcState.status) {
        traceProcessors = httpRpcState.status.traceProcessorStatuses ?? [];
      }

      // Filter to only show trace processors that have a loaded trace
      const processorsWithTraces = traceProcessors.filter(
        (tp) => tp.status?.loadedTraceName && tp.status.loadedTraceName !== '',
      );

      // Sort processors: those without active tabs first, then those with active tabs
      const sortedProcessors = [...processorsWithTraces].sort((a, b) => {
        const aHasTab = a.status?.hasExistingTab ?? false;
        const bHasTab = b.status?.hasExistingTab ?? false;
        return aHasTab === bHasTab ? 0 : aHasTab ? 1 : -1;
      });

      showModal({
        title: 'Use trace processor native acceleration?',
        content: () => {
          const elements: m.Child[] = [
            m(
              'p',
              `Trace Processor detected on ${HttpRpcEngine.hostAndPort} with loaded traces including ${tpStatus.loadedTraceName}.`,
            ),
          ];

          // Option 1: Select from loaded traces (only shown if available)
          if (processorsWithTraces.length > 0) {
            elements.push(
              m('h4', 'Select a loaded trace:'),

              // Add warning banner for active tabs
              (() => {
                const activeTabCount = sortedProcessors.filter(
                  (tp) => tp.status?.hasExistingTab ?? false,
                ).length;

                if (activeTabCount > 0) {
                  return m(
                    '.warning-banner',
                    {
                      style: {
                        'background-color': '#fff3cd',
                        'border': '1px solid #ffeaa7',
                        'border-radius': '4px',
                        'padding': '8px 12px',
                        'margin': '8px 0 16px 0',
                        'color': '#856404',
                        'font-size': '13px',
                      },
                    },
                    [
                      m('strong', '⚠️ Warning: '),
                      'Each loaded trace can be opened in at most one tab at a time.',
                    ],
                  );
                }
                return null;
              })(),

              // Interactive trace processor selection
              m('div', {style: {margin: '8px 0'}}, [
                sortedProcessors.map((tp, index) => {
                  const status = tp.status!;
                  const hasActiveTab = status.hasExistingTab ?? false;

                  return m(
                    'button',
                    {
                      key: tp.uuid || `default-${index}`,
                      style: {
                        'display': 'block',
                        'width': '100%',
                        'padding': '12px',
                        'margin': '8px 0',
                        'border': '1px solid #ddd',
                        'border-radius': '4px',
                        'background-color': hasActiveTab
                          ? '#fff3cd'
                          : '#f8f9fa',
                        'cursor': 'pointer',
                        'opacity': hasActiveTab ? '1.0' : '1',
                        'text-align': 'left',
                        'transition': 'background-color 0.2s ease',
                      },
                      onmouseenter: function (this: HTMLElement) {
                        this.style.backgroundColor = hasActiveTab
                          ? '#ffeaa7'
                          : '#e2e6ea';
                      },
                      onmouseleave: function (this: HTMLElement) {
                        this.style.backgroundColor = hasActiveTab
                          ? '#fff3cd'
                          : '#f8f9fa';
                      },
                      onclick: () => {
                        if (tp.uuid) {
                          AppImpl.instance.httpRpc.selectedTraceProcessorUuid =
                            tp.uuid;
                          closeModal();
                          resolve(
                            PreloadedDialogResult.UseRpcWithPreloadedTrace,
                          );
                        } else {
                          // Handle case where uuid is null/undefined
                          closeModal();
                          resolve(
                            PreloadedDialogResult.UseRpcWithPreloadedTrace,
                          );
                        }
                      },
                    },
                    [
                      m('div', [
                        m('strong', status.loadedTraceName),
                        m('br'),
                        m('small', `UUID: ${tp.uuid || 'default'}`),
                        m('br'),
                        m(
                          'small',
                          `Version: ${status.humanReadableVersion || 'unknown'}`,
                        ),
                      ]),
                      hasActiveTab &&
                        m(
                          'span',
                          {
                            style: {
                              'color': '#d63384',
                              'font-weight': 'bold',
                              'font-size': '11px',
                              'display': 'block',
                              'margin-top': '4px',
                            },
                          },
                          '⚠️ Active tab exists - close the old tab first before loading to prevent crashing',
                        ),
                    ],
                  );
                }),
              ]),
            );
          }

          // Add explanatory text section for the other options
          elements.push(
            m(
              'div',
              {
                style: {
                  'margin-top': '20px',
                  'padding': '16px',
                  'background-color': '#f8f9fa',
                  'border-radius': '4px',
                  'border-left': '4px solid #007bff',
                },
              },
              [
                m('h4', {style: {'margin-top': '0'}}, 'Other Options:'),
                m('div', {style: {'margin-bottom': '12px'}}, [
                  m('strong', 'Load new trace:'),
                  m('br'),
                  m(
                    'small',
                    'Use this if you want to open another trace but still use the accelerator.',
                  ),
                ]),
                m('div', {style: {'margin-bottom': '12px'}}, [
                  m('strong', 'Use built-in WASM:'),
                  m('br'),
                  m('small', 'Will not use the accelerator in this tab.'),
                ]),
              ],
            ),
          );

          // Caveats section
          elements.push(
            m(
              'div',
              {
                style: {
                  'margin-top': '20px',
                  'padding': '12px',
                  'background-color': '#f8f9fa',
                  'border-left': '4px solid #007bff',
                  'border-radius': '4px',
                },
              },
              [
                m(
                  'strong',
                  'Using the native accelerator has some minor caveats:',
                ),
                m(
                  'ul',
                  {style: {'margin': '8px 0 0 20px', 'font-size': '14px'}},
                  [
                    m(
                      'li',
                      "Sharing, downloading and conversion-to-legacy aren't supported.",
                    ),
                    m(
                      'li',
                      'Each trace file can be opened in at most one tab at a time.',
                    ),
                  ],
                ),
              ],
            ),
          );

          return elements;
        },
        buttons: [
          // Main action buttons moved to footer
          {
            text: 'Load new trace',
            action: () => {
              closeModal();
              resolve(PreloadedDialogResult.UseRpc);
            },
          },
          {
            text: 'Use built-in WASM',
            action: () => {
              closeModal();
              resolve(PreloadedDialogResult.UseWasm);
            },
          },
          {
            text: 'Cancel',
            action: () => {
              closeModal();
              resolve(PreloadedDialogResult.Dismissed);
            },
          },
        ],
      });
    } catch (error) {
      console.error('Error showing trace processor selection modal:', error);
      resolve(PreloadedDialogResult.UseWasm);
    }
  });
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
