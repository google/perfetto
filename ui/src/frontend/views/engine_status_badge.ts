// Copyright (C) 2026 The Android Open Source Project
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
import type {EngineMode} from '../../trace_processor/engine';
import type {AppImpl} from '../../core/app_impl';

export interface EngineStatusBadgeAttrs {
  readonly app: AppImpl;
}

export const EngineStatusBadge: m.Component<EngineStatusBadgeAttrs> = {
  view({attrs}: m.CVnode<EngineStatusBadgeAttrs>) {
    const engine = attrs.app.trace?.engine;
    const failed = engine?.failed !== undefined;

    // Resolve the engine mode. If no engine exists yet, guess based on the
    // current httpRpc state; this may be wrong (trace_controller.ts has the
    // final say) but will reconcile once the engine is created.
    const mode: EngineMode = resolveMode(attrs.app, engine?.mode);

    let title = 'Number of pending SQL queries';
    let modifier: string | undefined;
    let label: string;

    if (failed) {
      modifier = 'pf-sidebar__dbg-info-square--red';
      title = 'Query engine crashed\n' + engine!.failed;
    }

    if (mode === 'HTTP_RPC') {
      if (!failed) modifier = 'pf-sidebar__dbg-info-square--green';
      label = 'RPC';
      title += '\n(Query engine: native accelerator over HTTP+RPC)';
    } else {
      label = 'WSM';
      title += '\n(Query engine: built-in WSM)';
    }

    return m(
      '.pf-sidebar__dbg-info-square',
      {className: modifier, title},
      m('div', label),
      m('div', failed ? 'FAIL' : engine?.numRequestsPending ?? '-'),
    );
  },
};

function resolveMode(
  app: AppImpl,
  current: EngineMode | undefined,
): EngineMode {
  if (current !== undefined) return current;
  if (
    app.httpRpc.httpRpcAvailable &&
    app.httpRpc.newEngineMode === 'USE_HTTP_RPC_IF_AVAILABLE'
  ) {
    return 'HTTP_RPC';
  }
  return 'WASM';
}
