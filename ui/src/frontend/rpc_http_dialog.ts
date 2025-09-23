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
import {AppImpl} from '../core/app_impl';
import {Button} from '../widgets/button';
import {Callout} from '../widgets/callout';
import {Card, CardStack} from '../widgets/card';
import {Intent} from '../widgets/common';
import {EmptyState} from '../widgets/empty_state';
import {closeModal, showModal} from '../widgets/modal';
import {Stack, StackAuto} from '../widgets/stack';
import {classNames} from '../base/classnames';

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

function formatInactivity(nanos: number): string {
  // Convert nanoseconds → milliseconds
  const ms = nanos / 1_000_000;

  // Convert to seconds, minutes, hours, days
  const seconds = Math.floor(ms / 1000);
  const minutes = Math.floor(seconds / 60);
  const hours = Math.floor(minutes / 60);
  const days = Math.floor(hours / 24);

  if (days > 0) {
    return `${days} day${days > 1 ? 's' : ''} ago`;
  } else if (hours > 0) {
    return `${hours} hour${hours > 1 ? 's' : ''} ago`;
  } else if (minutes > 0) {
    return `${minutes} minute${minutes > 1 ? 's' : ''} ago`;
  } else {
    return `${seconds} second${seconds !== 1 ? 's' : ''} ago`;
  }
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

      // UI selection state: null means "New instance" (or nothing chosen).
      // If set to a number, it refers to an existing instanceId.
      let selectedInstanceId: number | null = null;

      showModal({
        title: 'Use trace processor native acceleration?',
        content: () => {
          const elements: m.Child[] = [];

          elements.push(
            m(
              'p',
              `Current active sessions on ${HttpRpcEngine.hostAndPort} (select one or pick "New instance" below if you want to open another trace):`,
            ),
          );

          // Filter to only show trace processors that have a loaded trace
          const processorsWithTraces = traceProcessors.filter(
            (tp) => (tp.loadedTraceName ?? '') !== '',
          );

          // Sort processors: those without active tabs first, then by instance ID.
          const sortedProcessors = [...processorsWithTraces].sort((a, b) => {
            const aHasTab = a.isAttached ?? false;
            const bHasTab = b.isAttached ?? false;

            if (aHasTab !== bHasTab) {
              return aHasTab ? 1 : -1;
            }

            const aId = a.instanceId ?? 0;
            const bId = b.instanceId ?? 0;
            return aId - bId;
          });

          if (processorsWithTraces.length > 0) {
            const activeTabCount = sortedProcessors.filter(
              (tp) => tp.isAttached ?? false,
            ).length;

            if (activeTabCount > 0) {
              elements.push(
                m(
                  Callout,
                  {
                    intent: Intent.Warning,
                    icon: 'warning',
                  },
                  'Each loaded trace can be opened in at most one tab at a time. If you want to open a trace that is already open in another tab, please close the old tab first and refresh.',
                ),
              );
            }

            // numbered list semantics for rows
            const rows = sortedProcessors.map((tp, index) => {
              const status = tp;
              const hasActiveTab = status.isAttached ?? false;
              const id = status.instanceId ?? null;
              const isSelected = id !== null && selectedInstanceId === id;

              const classes = classNames(
                'pf-rpc-http-dialog__row',
                isSelected && 'pf-rpc-http-dialog__row--selected',
                hasActiveTab && 'pf-rpc-http-dialog__row--disabled',
              );

              return m(
                Card as unknown as string,
                {
                  key: `row-${id ?? `default-${index}`}`,
                  role: 'option',
                  tabindex: hasActiveTab ? -1 : 0,
                  className: classes,
                  interactive: !hasActiveTab,
                  onclick: () => {
                    // do not allow selecting rows that already have an active tab
                    if (hasActiveTab) return;
                    selectedInstanceId = id ?? null;
                  },
                  onkeypress: (e: KeyboardEvent) => {
                    if (hasActiveTab) return;
                    if (e.key === 'Enter' || e.key === ' ') {
                      e.preventDefault();
                      selectedInstanceId = id ?? null;
                    }
                  },
                },
                m(
                  Stack,
                  {orientation: 'horizontal', spacing: 'small'},
                  // left side: text grows to fill available space
                  m(
                    StackAuto,
                    m(
                      'strong',
                      `#${status.instanceId ?? '0'} ${status.loadedTraceName ?? ''} ${formatInactivity(status.inactivityNs ?? 0)}${hasActiveTab ? ' [ATTACHED]' : ''}`,
                    ),
                  ),
                ),
                // right side: fixed button
                m(Button, {
                  icon: 'close',
                  title: 'Close this trace processor instance',
                  compact: true,
                  onclick: async (e: MouseEvent) => {
                    e.stopPropagation();
                    if (id === null) return;
                    await fetch(`http://${HttpRpcEngine.hostAndPort}/close`, {
                      method: 'POST',
                      body: String(id),
                    });
                    traceProcessors = traceProcessors.filter(
                      (p) => p.instanceId !== id,
                    );
                    m.redraw();
                  },
                }),
              );
            });

            const newInstanceSelected = selectedInstanceId === null;

            const newClasses = [
              'pf-rpc-http-dialog__row',
              newInstanceSelected ? 'pf-rpc-http-dialog__row--selected' : '',
            ].join(' ');

            elements.push(
              m(CardStack, [
                ...rows,
                m(
                  Card as unknown as string,
                  {
                    key: 'new-instance-row',
                    role: 'option',
                    tabindex: 0,
                    className: newClasses,
                    interactive: true,
                    onclick: () => {
                      selectedInstanceId = null;
                      m.redraw(); // force UI update
                    },
                    onkeypress: (e: KeyboardEvent) => {
                      if (e.key === 'Enter' || e.key === ' ') {
                        e.preventDefault();
                        selectedInstanceId = null;
                        m.redraw(); // force UI update
                      }
                    },
                  },
                  m(
                    Stack,
                    {orientation: 'horizontal'},
                    m(StackAuto, m('strong', 'New instance …')),
                  ),
                ),
              ]),
            );
          } else {
            elements.push(
              m(EmptyState, {
                title: `There are no current active sessions on ${HttpRpcEngine.hostAndPort}.`,
              }),
            );

            // ensure default selection is "New instance" if nothing exists
            selectedInstanceId = null;
          }

          return elements;
        },
        buttons: [
          {
            text: 'Yes, Attach to external RPC',
            primary: true,
            action: () => {
              if (selectedInstanceId !== null) {
                AppImpl.instance.httpRpc.selectedTraceProcessorId =
                  selectedInstanceId;
                closeModal();
                resolve(PreloadedDialogResult.UseRpcWithPreloadedTrace);
                return;
              }

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
