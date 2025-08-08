// Copyright (C) 2024 The Android Open Source Project
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
import {TracedWebsocketTarget} from './traced_websocket_target';
import {PreflightCheckRenderer} from '../pages/preflight_check_renderer';
import {closeModal, showModal} from '../../../widgets/modal';
import {Button} from '../../../widgets/button';
import {TracedWebsocketTargetProvider} from './traced_websocket_provider';
import {defer, Deferred} from '../../../base/deferred';

/**
 * Shows a dialog that allows to add a connection to another websocket endpoint
 * other than the default 127.0.0.1:8037. This dialog is displayed when the user
 * clicks on "connect new device" in the "Target Device" page.
 */
export async function showTracedConnectionManagementDialog(
  provider: TracedWebsocketTargetProvider,
): Promise<TracedWebsocketTarget | undefined> {
  const resultPromise = defer<TracedWebsocketTarget | undefined>();
  const key = 'TracedConnectioManagementDialog';
  showModal({
    key,
    title: 'Connect to remote tracing service',
    content: () =>
      m(TracedConnectioManagementDialog, {provider, resultPromise}),
  }).then(() => resultPromise.resolve(undefined));
  const targetOrUndefined = await resultPromise;
  closeModal(key);
  return targetOrUndefined;
}

interface DialogAttrs {
  provider: TracedWebsocketTargetProvider;
  resultPromise: Deferred<TracedWebsocketTarget | undefined>;
}
class TracedConnectioManagementDialog implements m.ClassComponent<DialogAttrs> {
  private target?: TracedWebsocketTarget;
  private checks?: PreflightCheckRenderer;

  view({attrs}: m.CVnode<DialogAttrs>) {
    const provider = attrs.provider;
    return m(
      '.pf-record-page',
      m(
        'div',
        'Forward port 8037 with ssh from the local host to the ' +
          'remote host where traced is running and invoke websocket_bridge.',
      ),
      m('br'),
      m(
        'code',
        "ssh -L8037:localhost:8037 <remote-machine> 'websocket_bridge'",
      ),
      m('header', 'Connect a new target'),
      m(
        'div',
        m('input', {
          placeholder: 'remote_machine:8037',
          onchange: (e: Event) =>
            this.testConnection((e.target as HTMLInputElement).value ?? ''),
        }),
        m(Button, {
          icon: 'add',
          onclick: () => {
            if (this.target !== undefined) {
              provider.targets.set(this.target.wsUrl, this.target);
            }
            attrs.resultPromise.resolve(this.target);
          },
        }),
      ),
      this.checks && this.checks.renderTable(),
      m('header', 'Manage targets'),
      m(
        'table',
        ...Array.from(provider.targets.entries()).map(([wsUrl, target]) =>
          m(
            'tr',
            m(
              'td',
              m(Button, {
                icon: 'delete',
                onclick: () => {
                  target.disconnect();
                  provider.targets.delete(wsUrl);
                  provider.onTargetsChanged.notify();
                },
              }),
            ),
            m('td', m('code', wsUrl)),
          ),
        ),
      ),
    );
  }

  private testConnection(userInput: string) {
    this.target && this.target.disconnect();
    this.target = undefined;
    this.checks = undefined;

    let wsUrl: string;
    if (userInput.match(/^ws(s?):\/\//)) {
      wsUrl = userInput;
    } else if (userInput.match(/^[^:/]+:\d+$/)) {
      wsUrl = `ws://${userInput}/traced`;
    } else if (userInput.match(/^[^:/]+$/)) {
      wsUrl = `ws://${userInput}:8037/traced`;
    } else {
      return;
    }

    this.target = new TracedWebsocketTarget(wsUrl);
    this.checks = new PreflightCheckRenderer(this.target);
    this.checks.runPreflightChecks();
  }
}
