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
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {CodeSnippet} from '../../widgets/code_snippet';
import {Intent} from '../../widgets/common';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import {
  allowOriginCommand,
  installCommand,
  tunnelCommand,
} from '../settings/local_server_setup';
import {
  activeServer,
  addServer,
  type BigtraceServer,
  isLoopbackEndpoint,
  LOCAL_ENDPOINT,
  listServers,
  removeServer,
  sameEndpoint,
  selectServer,
} from '../settings/server_list';
import {serverProbes} from '../settings/server_probe';

// How often loopback servers are asked whether they are up while the panel is
// open, so the local server shows up as soon as it starts.
const LOOPBACK_POLL_MS = 2000;

// Label of the topbar button: the server's name, else the endpoint's host.
function endpointLabel(endpoint: string): string {
  if (endpoint === '') return 'Connect backend';
  const server = activeServer();
  if (server !== undefined) return server.name;
  try {
    return new URL(endpoint).host;
  } catch {
    return endpoint;
  }
}

function reloadWith(endpoint: string): void {
  selectServer(endpoint);
  // Modules read the endpoint when they initialize.
  window.location.reload();
}

// The server is the one piece of state that isn't per-query — it's a
// connection, not a configuration — so it lives up here rather than in the
// per-tab settings. Auto-opens once when there's no working backend, which is
// the app's onboarding step now that the settings page is gone.
export class ConnectionButton implements m.ClassComponent {
  private open = false;
  private autoOpened = false;

  view(): m.Children {
    const endpoint = getBigtraceEndpoint();
    // The endpoint ships with a default, so "set" doesn't mean "reachable" —
    // a backend that failed to answer counts as not connected, otherwise this
    // button claims a connection the app doesn't have.
    const connected =
      endpoint !== '' &&
      bigTraceSettingsStorage.execConfigLoadError === undefined;
    if (
      !this.autoOpened &&
      !connected &&
      !bigTraceSettingsStorage.isExecConfigLoading
    ) {
      this.autoOpened = true;
      this.open = true;
    }
    return m(
      Popup,
      {
        trigger: m(Button, {
          icon: connected ? 'cloud_done' : 'cloud_off',
          label: endpointLabel(endpoint),
          title: connected
            ? endpoint
            : `Not connected to ${endpoint === '' ? 'a backend' : endpoint}`,
          onclick: () => {
            this.open = !this.open;
          },
        }),
        isOpen: this.open,
        onChange: (isOpen: boolean) => {
          this.open = isOpen;
        },
        position: PopupPosition.BottomEnd,
        className: 'pf-bt-connection-popup',
      },
      m(ServerPanel, {connected}),
    );
  }
}

interface ServerPanelAttrs {
  // Whether the active server answered the app.
  readonly connected: boolean;
}

// The server picker: the servers with whether they answer, a way to add one,
// and the guide that sets up the local server and connects once it is up.
class ServerPanel implements m.ClassComponent<ServerPanelAttrs> {
  private timer?: ReturnType<typeof setInterval>;
  private setupOpen = false;
  private adding = false;
  private newName = '';
  private newEndpoint = '';
  private addError?: string;

  oncreate() {
    for (const s of listServers()) void serverProbes.refresh(s.endpoint);
    this.timer = setInterval(() => this.poll(), LOOPBACK_POLL_MS);
  }

  onremove() {
    clearInterval(this.timer);
  }

  private async poll() {
    for (const s of listServers()) {
      if (!isLoopbackEndpoint(s.endpoint)) continue;
      const result = await serverProbes.refresh(s.endpoint);
      // The guide's last step: switch to the local server once it is up.
      if (
        this.setupOpen &&
        s.kind === 'local' &&
        result?.state === 'online' &&
        !sameEndpoint(getBigtraceEndpoint(), s.endpoint)
      ) {
        reloadWith(s.endpoint);
      }
    }
  }

  view({attrs}: m.Vnode<ServerPanelAttrs>): m.Children {
    const loadError = bigTraceSettingsStorage.execConfigLoadError;
    const endpoint = getBigtraceEndpoint();
    const servers = listServers();
    const known = servers.some((s) => sameEndpoint(s.endpoint, endpoint));
    return m('.pf-bt-connection-panel', [
      m('.pf-bt-connection-panel__title', 'BigTrace server'),
      m(
        '.pf-bt-connection-panel__desc',
        'The server that holds the traces and runs your queries.',
      ),
      m('.pf-bt-server-list', [
        servers.map((s) => this.renderServer(s, attrs.connected)),
        // An endpoint from ?server= or an older version that isn't listed.
        !known &&
          endpoint !== '' &&
          this.renderServer(
            {name: 'Current server', endpoint, kind: 'custom'},
            attrs.connected,
            /* unsaved= */ true,
          ),
      ]),
      this.adding ? this.renderAddForm() : this.renderActions(),
      this.setupOpen && this.renderSetup(),
      loadError !== undefined &&
        m(
          Callout,
          {intent: Intent.Danger, icon: 'error'},
          `Couldn't load settings from ${endpoint}: ${loadError}`,
        ),
      // A local server installed without this UI's origin refuses it.
      loadError?.includes('Origin not allowed') &&
        isLoopbackEndpoint(endpoint) && [
          m(
            '.pf-bt-local-setup__step',
            'This server does not accept requests from this page. On its ' +
              'machine, allow it with:',
          ),
          m(CodeSnippet, {text: allowOriginCommand(window.location.origin)}),
        ],
    ]);
  }

  private renderServer(
    s: BigtraceServer,
    connected: boolean,
    unsaved = false,
  ): m.Children {
    const active = sameEndpoint(s.endpoint, getBigtraceEndpoint());
    const probe = serverProbes.get(s.endpoint);
    const state = active && connected ? 'online' : (probe?.state ?? 'checking');
    const info = probe?.state === 'online' ? probe.info : undefined;
    const detail =
      info?.traceCount !== undefined
        ? `${info.traceCount} traces in ${info.traceDir ?? '?'}`
        : s.endpoint;
    return m(
      '.pf-bt-server',
      {
        'className': active ? 'pf-bt-server--active' : '',
        'data-server-kind': s.kind,
        'role': 'button',
        'tabindex': 0,
        'title': active ? `Connected to ${s.endpoint}` : `Use ${s.endpoint}`,
        'onclick': () => {
          if (!active) reloadWith(s.endpoint);
        },
        'onkeydown': (e: KeyboardEvent) => {
          if ((e.key === 'Enter' || e.key === ' ') && !active) {
            reloadWith(s.endpoint);
          }
        },
      },
      [
        m(`span.pf-bt-server__dot.pf-bt-server__dot--${state}`, {
          title: {
            online: 'Answering',
            offline: 'Not answering',
            checking: 'Checking…',
          }[state],
        }),
        m('.pf-bt-server__text', [
          m('.pf-bt-server__name', s.name),
          m('.pf-bt-server__detail', detail),
        ]),
        s.kind === 'local' &&
          state === 'offline' &&
          !this.setupOpen &&
          m(Button, {
            label: 'Set up',
            icon: 'build',
            compact: true,
            intent: Intent.Primary,
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              this.setupOpen = true;
            },
          }),
        unsaved &&
          m(Button, {
            icon: 'bookmark_add',
            title: 'Add to the list',
            compact: true,
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              this.adding = true;
              this.newEndpoint = s.endpoint;
            },
          }),
        s.kind === 'custom' &&
          !unsaved &&
          m(Button, {
            icon: 'delete',
            title: `Remove ${s.name}`,
            compact: true,
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              removeServer(s.endpoint);
            },
          }),
      ],
    );
  }

  private renderActions(): m.Children {
    return m('.pf-bt-connection-panel__actions', [
      m(Button, {
        label: 'Add server',
        icon: 'add',
        onclick: () => {
          this.adding = true;
          this.addError = undefined;
        },
      }),
    ]);
  }

  private renderAddForm(): m.Children {
    const submit = () => {
      try {
        const server = addServer(this.newName, this.newEndpoint);
        this.adding = false;
        this.newName = this.newEndpoint = '';
        this.addError = undefined;
        void serverProbes.refresh(server.endpoint);
      } catch (e) {
        this.addError = e instanceof Error ? e.message : String(e);
      }
    };
    return m(
      'form.pf-bt-server-form',
      {
        onsubmit: (e: Event) => {
          e.preventDefault();
          submit();
        },
      },
      [
        m(TextInput, {
          value: this.newName,
          placeholder: 'Name (optional), e.g. Alice’s workstation',
          className: 'pf-bt-connection-panel__input',
          oninput: (e: Event) => {
            this.newName = (e.target as HTMLInputElement).value;
          },
        }),
        m(TextInput, {
          value: this.newEndpoint,
          placeholder: 'http://127.0.0.1:5002/v1',
          className: 'pf-bt-connection-panel__input',
          autofocus: true,
          oninput: (e: Event) => {
            this.newEndpoint = (e.target as HTMLInputElement).value;
            this.addError = undefined;
          },
        }),
        this.addError !== undefined &&
          m(Callout, {intent: Intent.Danger, icon: 'error'}, this.addError),
        m('.pf-bt-connection-panel__actions', [
          m(Button, {
            label: 'Cancel',
            type: 'button',
            onclick: () => {
              this.adding = false;
              this.addError = undefined;
            },
          }),
          m(Button, {
            label: 'Add',
            icon: 'add',
            type: 'submit',
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            disabled: this.newEndpoint.trim() === '',
          }),
        ]),
      ],
    );
  }

  private renderSetup(): m.Children {
    const local = serverProbes.get(LOCAL_ENDPOINT);
    return m('.pf-bt-local-setup', [
      m('.pf-bt-local-setup__title', 'Set up the local server'),
      m(
        '.pf-bt-local-setup__step',
        '1. On the Linux machine with your traces (e.g. your workstation), ' +
          'install the always-on server. It serves ~/traces by default ' +
          '(add -- --traces DIR to change it) and keeps running across ' +
          'restarts; `bigtrace status`, `stop`, `start` and `logs` control it.',
      ),
      m(CodeSnippet, {text: installCommand(window.location.origin)}),
      m(
        '.pf-bt-local-setup__step',
        '2. If this browser runs on another machine (e.g. a laptop), forward ' +
          'the port from it, and keep that running:',
      ),
      m(CodeSnippet, {text: tunnelCommand()}),
      m(
        '.pf-bt-local-setup__hint',
        'or, with the bigtrace command installed there too: ' +
          '`bigtrace tunnel YOUR_WORKSTATION`. Your browser may ask to let ' +
          'this site access your local network: allow it.',
      ),
      m('.pf-bt-local-setup__status', [
        local?.state === 'online'
          ? 'Local server found. Connecting…'
          : [m(Spinner), ` Waiting for ${LOCAL_ENDPOINT}…`],
      ]),
      m('.pf-bt-connection-panel__actions', [
        m(Button, {
          label: 'Close',
          onclick: () => {
            this.setupOpen = false;
          },
        }),
      ]),
    ]);
  }
}
