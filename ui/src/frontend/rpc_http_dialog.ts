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
import {Actions} from '../common/actions';
import {HttpRpcEngine, RPC_URL} from '../common/http_rpc_engine';
import {VERSION} from '../gen/perfetto_version';
import {StatusResult, TraceProcessorApiVersion} from '../protos';

import {globals} from './globals';
import {showModal} from './modal';

const CURRENT_API_VERSION =
    TraceProcessorApiVersion.TRACE_PROCESSOR_CURRENT_API_VERSION;

const PROMPT = `Trace Processor Native Accelerator detected on ${RPC_URL} with:
$loadedTraceName

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
- You may encounter UI errors if the Trace Processor version you are using is
too old. Get the latest version from get.perfetto.dev/trace_processor.
`;


const MSG_TOO_OLD = `The Trace Processor instance on ${RPC_URL} is too old.

This UI requires TraceProcessor features that are not present in the
Trace Processor native accelerator you are currently running.
If you continue, this is almost surely going to cause UI failures.

Please update your local Trace Processor binary:

curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
./trace_processor --httpd

UI version: ${VERSION}
TraceProcessor RPC API required: ${CURRENT_API_VERSION} or higher

TraceProcessor version: $tpVersion
RPC API: $tpApi
`;

let forceUseOldVersion = false;

// Try to connect to the external Trace Processor HTTP RPC accelerator (if
// available, often it isn't). If connected it will populate the
// |httpRpcState| in the frontend local state. In turn that will show the UI
// chip in the sidebar. trace_controller.ts will repeat this check before
// trying to load a new trace. We do this ahead of time just to have a
// consistent UX (i.e. so that the user can tell if the RPC is working without
// having to open a trace).
export async function CheckHttpRpcConnection(): Promise<void> {
  const state = await HttpRpcEngine.checkConnection();
  globals.frontendLocalState.setHttpRpcState(state);
  if (!state.connected) return;
  const tpStatus = assertExists(state.status);

  if (tpStatus.apiVersion < CURRENT_API_VERSION) {
    await showDialogTraceProcessorTooOld(tpStatus);
    if (!forceUseOldVersion) return;
  }

  if (tpStatus.loadedTraceName) {
    // If a trace is already loaded in the trace processor (e.g., the user
    // launched trace_processor_shell -D trace_file.pftrace), prompt the user to
    // initialize the UI with the already-loaded trace.
    return showDialogToUsePreloadedTrace(tpStatus);
  }
}

async function showDialogTraceProcessorTooOld(tpStatus: StatusResult) {
  return showModal({
    title: 'Your Trace Processor binary is outdated',
    content:
        m('.modal-pre',
          MSG_TOO_OLD.replace('$tpVersion', tpStatus.humanReadableVersion)
              .replace('$tpApi', `${tpStatus.apiVersion}`)),
    buttons: [
      {
        text: 'Use builtin Wasm',
        primary: true,
        action: () => {
          globals.dispatch(
              Actions.setNewEngineMode({mode: 'FORCE_BUILTIN_WASM'}));
        },
      },
      {
        text: 'Use old version regardless (might crash)',
        primary: false,
        action: () => {
          forceUseOldVersion = true;
        },
      },
    ],
  });
}

async function showDialogToUsePreloadedTrace(tpStatus: StatusResult) {
  return showModal({
    title: 'Use Trace Processor Native Acceleration?',
    content:
        m('.modal-pre',
          PROMPT.replace('$loadedTraceName', tpStatus.loadedTraceName)),
    buttons: [
      {
        text: 'YES, use loaded trace',
        primary: true,
        action: () => {
          globals.dispatch(Actions.openTraceFromHttpRpc({}));
        },
      },
      {
        text: 'YES, but reset state',
      },
      {
        text: 'NO, Use builtin Wasm',
        action: () => {
          globals.dispatch(
              Actions.setNewEngineMode({mode: 'FORCE_BUILTIN_WASM'}));
        },
      },
    ],
  });
}
