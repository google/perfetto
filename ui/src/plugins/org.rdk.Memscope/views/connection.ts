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
import {assertUnreachable} from '../../../base/assert';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {RadioGroup} from '../../../widgets/radio_group';
import {TextInput} from '../../../widgets/text_input';
import type {App} from '../../../public/app';
import {getSharedMsgChannelTargetRegistry} from '../../org.rdk.RecordTrace/msgchannel_target_registry';
import type {MsgChannelTarget} from '../../org.rdk.RecordTrace/msgchannel_target_registry';
import {TracedMsgChannelTarget} from '../../org.rdk.RecordTrace/traced_over_msgchannel/traced_msgchannel_target';
import {TracedWebsocketTarget} from '../../org.rdk.RecordTrace/traced_over_websocket/traced_websocket_target';
import {Page} from '../components/page';
import {Hero} from '../components/hero';
import {PreviewBanner} from '../components/preview_banner';

export interface ConnectionResult {
  deviceName: string;
  linuxTarget?: TracedWebsocketTarget | TracedMsgChannelTarget;
}

interface ConnectionPageAttrs {
  readonly app: App;
  readonly onConnected: (result: ConnectionResult) => void;
}

type ConnectionMethod = 'websocket' | 'msgchannel';

export class ConnectionPage implements m.ClassComponent<ConnectionPageAttrs> {
  private error?: string;
  private connectionMethod: ConnectionMethod = 'websocket';

  // Traced-over-WebSocket state.
  private wsConnecting = false;
  private wsUrl = '127.0.0.1:8037';

  // MessageChannel state.
  private readonly msgChannelRegistry = getSharedMsgChannelTargetRegistry();
  private msgChannelTargets: MsgChannelTarget[] = [];
  private msgChannelConnectingSession?: string;

  constructor() {
    this.msgChannelTargets = [...this.msgChannelRegistry.providers];
    this.msgChannelRegistry.onProviderRegistered.addListener((target) => {
      this.msgChannelTargets.push(target);
      m.redraw();
    });
  }

  view({attrs}: m.CVnode<ConnectionPageAttrs>) {
    return m(
      Page,
      m(Page.Title, 'Memscope'),
      m(PreviewBanner, {app: attrs.app}),
      m(
        Hero,
        m(Hero.Icon, {icon: 'memory'}),
        m(
          Hero.Text,
          'Connect to an Android device or Linux host to monitor ' +
            'per-process memory usage in real time via traced.',
        ),
        m(
          RadioGroup,
          {
            intent: Intent.Primary,
            selectedValue: this.connectionMethod,
            onValueChange: (value) => {
              this.connectionMethod = value as ConnectionMethod;
              this.error = undefined;
            },
          },
          m(RadioGroup.Button, {value: 'websocket', icon: 'lan'}, 'WebSocket'),
          m(
            RadioGroup.Button,
            {value: 'msgchannel', icon: 'swap_horiz'},
            'MessageChannel',
          ),
        ),
        this.renderConnectBox(attrs),
        this.error && m('.pf-memscope-error', this.error),
      ),
    );
  }

  private renderConnectBox(attrs: ConnectionPageAttrs): m.Children {
    switch (this.connectionMethod) {
      case 'websocket':
        return this.renderWsConnect(attrs);
      case 'msgchannel':
        return this.renderMsgChannelConnect(attrs);
      default:
        assertUnreachable(this.connectionMethod);
    }
  }

  private renderWsConnect(attrs: ConnectionPageAttrs): m.Children {
    return [
      m(TextInput, {
        placeholder: 'hostname:8037',
        value: this.wsUrl,
        leftIcon: 'lan',
        onInput: (value: string) => {
          this.wsUrl = value;
        },
        disabled: this.wsConnecting,
      }),
      m(Button, {
        label: this.wsConnecting ? 'Connecting...' : 'Connect to traced',
        icon: 'lan',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.wsConnecting || this.wsUrl.trim() === '',
        onclick: () => this.connectWebsocket(attrs),
      }),
    ];
  }

  private renderMsgChannelConnect(attrs: ConnectionPageAttrs): m.Children {
    if (this.msgChannelTargets.length === 0) {
      return m(
        '.pf-memscope-hero__text',
        'No MessageChannel targets found. Open Memscope from a page that provides a session.',
      );
    }

    return m(
      '.pf-memscope-device-list',
      this.msgChannelTargets.map((target) =>
        m(Button, {
          key: `${target.srcDomain}`,
          label:
            this.msgChannelConnectingSession === target.session
              ? 'Connecting...'
              : `${target.srcDomain}`,
          icon: 'swap_horiz',
          variant: ButtonVariant.Outlined,
          disabled: this.msgChannelConnectingSession !== undefined,
          onclick: () => this.connectMsgChannelTarget(attrs, target),
        }),
      ),
    );
  }

  private parseWsUrl(userInput: string): string | undefined {
    const trimmed = userInput.trim();
    if (trimmed.match(/^wss?:\/\//)) {
      return trimmed;
    } else if (trimmed.match(/^[^:/]+:\d+$/)) {
      return `ws://${trimmed}/traced`;
    } else if (trimmed.match(/^[^:/]+$/)) {
      return `ws://${trimmed}:8037/traced`;
    }
    return undefined;
  }

  private async connectWebsocket(attrs: ConnectionPageAttrs) {
    const wsUrl = this.parseWsUrl(this.wsUrl);
    if (wsUrl === undefined) {
      this.error = 'Invalid URL. Use hostname:port or ws://hostname:port/path';
      return;
    }

    this.wsConnecting = true;
    this.error = undefined;
    m.redraw();

    try {
      const target = new TracedWebsocketTarget(wsUrl);
      for await (const check of target.runPreflightChecks()) {
        if (check.status.ok === false) {
          this.error = `${check.name}: ${check.status.error}`;
          this.wsConnecting = false;
          m.redraw();
          return;
        }
      }

      const host = this.wsUrl
        .trim()
        .replace(/^wss?:\/\//, '')
        .split('/')[0];
      this.wsConnecting = false;
      attrs.onConnected({
        linuxTarget: target,
        deviceName: `WebSocket (${host})`,
      });
      m.redraw();
    } catch (e) {
      this.error = `WebSocket connection failed: ${e}`;
      this.wsConnecting = false;
      m.redraw();
    }
  }

  private async connectMsgChannelTarget(
    attrs: ConnectionPageAttrs,
    target: MsgChannelTarget,
  ) {
    this.msgChannelConnectingSession = target.session;
    this.error = undefined;
    m.redraw();

    try {
      const msgChannelTarget = new TracedMsgChannelTarget(
        target.srcWindow,
        target.srcDomain,
        target.session,
      );

      for await (const check of msgChannelTarget.runPreflightChecks()) {
        if (check.status.ok === false) {
          this.error = `${check.name}: ${check.status.error}`;
          this.msgChannelConnectingSession = undefined;
          m.redraw();
          return;
        }
      }

      this.msgChannelConnectingSession = undefined;
      attrs.onConnected({
        linuxTarget: msgChannelTarget,
        deviceName: `MessageChannel (${target.srcDomain})`,
      });
      m.redraw();
    } catch (e) {
      this.error = `MessageChannel connection failed: ${e}`;
      this.msgChannelConnectingSession = undefined;
      m.redraw();
    }
  }
}
