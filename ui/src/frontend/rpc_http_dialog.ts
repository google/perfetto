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
import {Callout} from '../widgets/callout';
import {Card, CardStack} from '../widgets/card';
import {Intent} from '../widgets/common';
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
  function forceWasm() {
    AppImpl.instance.httpRpc.newEngineMode = 'FORCE_BUILTIN_WASM';
  }

  if (tpStatusAll.instances.length > 0) {
    const firstTpStatusData = tpStatusAll.instances[0];
    const firstTpStatus = protos.StatusResult.create(firstTpStatusData);
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
  }

  const result = await showDialogToUsePreloadedTrace();
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

async function showDialogToUsePreloadedTrace(): Promise<PreloadedDialogResult> {
  return new Promise<PreloadedDialogResult>(async (resolve) => {
    try {
      // Fetch actual trace processor data for the comprehensive modal
      const httpRpcState = await HttpRpcEngine.checkConnection();

      let traceProcessors: Array<protos.IStatusResult> = [];
      if (httpRpcState.connected && httpRpcState.status) {
        traceProcessors = httpRpcState.status.instances ?? [];
      }

      // Filter to only show trace processors that have a loaded trace
      const processorsWithTraces = traceProcessors.filter(
        (tp) => (tp.loadedTraceName ?? '') !== '',
      );

      // Sort processors: those without active tabs first, then those with active tabs
      const sortedProcessors = [...processorsWithTraces].sort((a, b) => {
        const aHasTab = a.hasExistingTab ?? false;
        const bHasTab = b.hasExistingTab ?? false;
        return aHasTab === bHasTab ? 0 : aHasTab ? 1 : -1;
      });

      showModal({
        title: 'Use trace processor native acceleration?',
        content: () => {
          const elements: m.Child[] = [
            m(
              'p',
              `Current active sessions on ${HttpRpcEngine.hostAndPort} (choose one or pick another option below):`,
            ),
          ];

          if (processorsWithTraces.length > 0) {
            const activeTabCount = sortedProcessors.filter(
              (tp) => tp.hasExistingTab ?? false,
            ).length;

            if (activeTabCount > 0) {
              elements.push(
                m(
                  Callout,
                  {
                    intent: Intent.Warning,
                    icon: 'warning',
                  },
                  'Each loaded trace can be opened in at most one tab at a time.',
                ),
              );
            }

            elements.push(
              m(
                CardStack,
                sortedProcessors.map((tp, index) => {
                  const status = tp;
                  const hasActiveTab = status.hasExistingTab ?? false;

                  return m(
                    Card,
                    {
                      key: tp.instanceId ?? `default-${index}`,
                      interactive: true,
                      onclick: () => {
                        if (tp.instanceId != null) {
                          AppImpl.instance.httpRpc.selectedTraceProcessorId =
                            tp.instanceId;
                        }
                        closeModal();
                        resolve(PreloadedDialogResult.UseRpcWithPreloadedTrace);
                      },
                    },
                    [
                      m('div', [
                        m('strong', status.loadedTraceName),
                        m('br'),
                        m(
                          'small',
                          {style: {color: 'var(--pf-color-text-muted)'}},
                          `ID: ${tp.instanceId ?? 'default'}`,
                        ),
                        m('br'),
                        m(
                          'small',
                          {style: {color: 'var(--pf-color-text-muted)'}},
                          `Version: ${status.humanReadableVersion || 'unknown'}`,
                        ),
                      ]),
                      hasActiveTab &&
                        m(
                          'div',
                          {
                            style: {
                              'color': 'var(--pf-color-danger)',
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
              ),
            );
          } else {
            elements.push(
              m(
                'p',
                {
                  style: {
                    'margin': '8px 0',
                    'font-style': 'italic',
                    'color': 'var(--pf-color-text-muted)',
                  },
                },
                `There are no current active sessions on ${HttpRpcEngine.hostAndPort}.`,
              ),
            );
          }

          elements.push(
            m('div', {style: {'margin-top': '20px'}}, [
              m('h4', 'Other Options:'),
              m('strong', 'Yes, Attach to external RPC:'),
              m(
                'ul',
                {style: {'margin-left': '20px', 'margin-top': '4px'}},
                m(
                  'li',
                  m(
                    'small',
                    {style: {color: 'var(--pf-color-text-muted)'}},
                    'Use this if you want to open another trace but still use the accelerator.',
                  ),
                ),
              ),
              m(
                'strong',
                {style: {'margin-top': '8px', 'display': 'block'}},
                'Use built-in WASM:',
              ),
              m(
                'ul',
                {style: {'margin-left': '20px', 'margin-top': '4px'}},
                m(
                  'li',
                  m(
                    'small',
                    {style: {color: 'var(--pf-color-text-muted)'}},
                    'Will not use the accelerator in this tab.',
                  ),
                ),
              ),
              m(
                'strong',
                {style: {'margin-top': '16px', 'display': 'block'}},
                'Using the native accelerator has some minor caveats:',
              ),
              m('ul', {style: {'margin-left': '20px', 'margin-top': '4px'}}, [
                m(
                  'li',
                  m(
                    'small',
                    {style: {color: 'var(--pf-color-text-muted)'}},
                    "Sharing, downloading and conversion-to-legacy aren't supported.",
                  ),
                ),
                m(
                  'li',
                  m(
                    'small',
                    {style: {color: 'var(--pf-color-text-muted)'}},
                    'Each trace file can be opened in at most one tab at a time.',
                  ),
                ),
              ]),
            ]),
          );

          return elements;
        },
        buttons: [
          // Main action buttons moved to footer
          {
            text: 'Yes, Attach to external RPC',
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
