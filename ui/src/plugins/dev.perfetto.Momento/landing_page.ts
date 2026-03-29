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
import {assertUnreachable} from '../../base/assert';
import {showPopupWindow} from '../../base/popup_window';
import {exists} from '../../base/utils';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {AdbDevice} from '../dev.perfetto.RecordTraceV2/adb/adb_device';
import {
  WDP_TRACK_DEVICES_SCHEMA,
  type WdpDevice,
} from '../dev.perfetto.RecordTraceV2/adb/web_device_proxy/wdp_schema';
import {AdbWebsocketDevice} from '../dev.perfetto.RecordTraceV2/adb/websocket/adb_websocket_device';
import {adbCmdAndWait} from '../dev.perfetto.RecordTraceV2/adb/websocket/adb_websocket_utils';
import {AdbKeyManager} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_key_manager';
import {AdbWebusbDevice} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_device';
import {
  ADB_DEVICE_FILTER,
  getAdbWebUsbInterface,
} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_utils';
import {TracedWebsocketTarget} from '../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';
import {AsyncWebsocket} from '../dev.perfetto.RecordTraceV2/websocket/async_websocket';

export interface ConnectionResult {
  device?: AdbDevice;
  deviceName: string;
  linuxTarget?: TracedWebsocketTarget;
}

interface LandingPageAttrs {
  onConnected: (result: ConnectionResult) => void;
}

type ConnectionMethod = 'usb' | 'websocket' | 'web_proxy' | 'linux';

interface WsDevice {
  serial: string;
  model: string;
}

export class LandingPage implements m.ClassComponent<LandingPageAttrs> {
  private adbKeyMgr = new AdbKeyManager();
  private connecting = false;
  private error?: string;
  private connectionMethod: ConnectionMethod = 'usb';

  // WebSocket bridge state.
  private wsDevices: WsDevice[] = [];
  private wsConnecting = false;
  private wsConnected = false;

  // WDP (Web Device Proxy) state.
  private wdpDevices: WdpDevice[] = [];
  private wdpSocket?: WebSocket;
  private wdpConnecting = false;
  private wdpConnected = false;

  onremove() {
    this.disconnectWdp();
  }

  view({attrs}: m.CVnode<LandingPageAttrs>) {
    return m(
      '.pf-live-memory-page__container',
      m(
        '.pf-live-memory-page',
        m('.pf-live-memory-title-bar', m('h1', 'Memento')),
        m(
          '.pf-live-memory-hero',
          m(Icon, {icon: 'memory', className: 'pf-live-memory-hero__icon'}),
          m(
            '.pf-live-memory-hero__text',
            'Connect to an Android device or Linux host to monitor ' +
              'per-process memory usage in real time via traced.',
          ),
          m(SegmentedButtons, {
            options: [
              {label: 'USB', icon: 'usb'},
              {label: 'WebSocket', icon: 'lan'},
              {label: 'Web Proxy', icon: 'corporate_fare'},
              {label: 'Linux', icon: 'computer'},
            ],
            selectedOption: (
              ['usb', 'websocket', 'web_proxy', 'linux'] as const
            ).indexOf(this.connectionMethod),
            onOptionSelected: (i: number) => {
              const methods: ConnectionMethod[] = [
                'usb',
                'websocket',
                'web_proxy',
                'linux',
              ];
              this.connectionMethod = methods[i];
              this.error = undefined;
            },
          }),
          this.renderConnectBox(attrs),
          this.error && m('.pf-live-memory-error', this.error),
        ),
      ),
    );
  }

  private renderConnectBox(attrs: LandingPageAttrs): m.Children {
    switch (this.connectionMethod) {
      case 'usb':
        return this.renderUsbConnect(attrs);
      case 'websocket':
        return this.renderWsConnect(attrs);
      case 'web_proxy':
        return this.renderWdpConnect(attrs);
      case 'linux':
        return this.renderLinuxConnect(attrs);
      default:
        assertUnreachable(this.connectionMethod);
    }
  }

  private renderUsbConnect(attrs: LandingPageAttrs): m.Children {
    return [
      m(Button, {
        label: this.connecting ? 'Connecting...' : 'Connect USB device',
        icon: 'usb',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.connecting || !exists(navigator.usb),
        onclick: () => this.connectDevice(attrs),
      }),
      !exists(navigator.usb) &&
        m('.pf-live-memory-error', 'WebUSB is not available in this browser.'),
    ];
  }

  private renderWsConnect(attrs: LandingPageAttrs): m.Children {
    if (!this.wsConnected) {
      return m(Button, {
        label: this.wsConnecting
          ? 'Connecting...'
          : 'Connect to WebSocket bridge',
        icon: 'lan',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.wsConnecting,
        onclick: () => this.connectWebsocket(),
      });
    }

    if (this.wsDevices.length === 0) {
      return m(
        '.pf-live-memory-hero__text',
        'No devices found. Connect an Android device via ADB.',
      );
    }

    return m(
      '.pf-live-memory-device-list',
      this.wsDevices.map((dev) =>
        m(Button, {
          key: dev.serial,
          label: this.connecting
            ? 'Connecting...'
            : `${dev.model} [${dev.serial}]`,
          icon: 'smartphone',
          variant: ButtonVariant.Outlined,
          disabled: this.connecting,
          onclick: () => this.connectWsDevice(attrs, dev),
        }),
      ),
    );
  }

  private renderWdpConnect(attrs: LandingPageAttrs): m.Children {
    if (!this.wdpConnected) {
      return m(Button, {
        label: this.wdpConnecting
          ? 'Connecting to proxy...'
          : 'Connect to Web Device Proxy',
        icon: 'corporate_fare',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.wdpConnecting,
        onclick: () => this.connectWdp(),
      });
    }

    if (this.wdpDevices.length === 0) {
      return m(
        '.pf-live-memory-hero__text',
        'No devices found. Connect an Android device and authorize it.',
      );
    }

    return m(
      '.pf-live-memory-device-list',
      this.wdpDevices.map((dev) => {
        const ready = dev.proxyStatus === 'ADB' && dev.adbStatus === 'DEVICE';
        const model =
          dev.proxyStatus === 'ADB' ? dev.adbProps?.model ?? '?' : '?';
        const label = ready
          ? `${model} [${dev.serialNumber}]`
          : `${dev.proxyStatus}/${dev.adbStatus} [${dev.serialNumber}]`;
        return m(Button, {
          key: dev.serialNumber,
          label: this.connecting ? 'Connecting...' : label,
          icon: ready ? 'smartphone' : 'lock',
          variant: ButtonVariant.Outlined,
          disabled: this.connecting,
          onclick: () => this.connectWdpDevice(attrs, dev),
        });
      }),
    );
  }

  private renderLinuxConnect(attrs: LandingPageAttrs): m.Children {
    return m(Button, {
      label: this.connecting ? 'Connecting...' : 'Connect to local traced',
      icon: 'computer',
      variant: ButtonVariant.Filled,
      intent: Intent.Primary,
      disabled: this.connecting,
      onclick: () => this.connectLinux(attrs),
    });
  }

  // ---------------------------------------------------------------------------
  // Connection methods
  // ---------------------------------------------------------------------------

  private async connectDevice(attrs: LandingPageAttrs) {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    try {
      const usbdev = await navigator.usb.requestDevice({
        filters: [ADB_DEVICE_FILTER],
      });

      const usbiface = getAdbWebUsbInterface(usbdev);
      if (!usbiface) {
        this.error = 'Could not find ADB interface on selected device.';
        this.connecting = false;
        m.redraw();
        return;
      }

      const result = await AdbWebusbDevice.connect(usbdev, this.adbKeyMgr);
      if (!result.ok) {
        this.error = result.error;
        this.connecting = false;
        m.redraw();
        return;
      }

      this.connecting = false;
      attrs.onConnected({
        device: result.value,
        deviceName: `${usbdev.productName} [${usbdev.serialNumber}]`,
      });
      m.redraw();
    } catch (e) {
      if (`${(e as {name?: string}).name}` === 'NotFoundError') {
        this.connecting = false;
        m.redraw();
        return;
      }
      this.error = `Connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
    }
  }

  private async connectLinux(attrs: LandingPageAttrs) {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    const wsUrl = 'ws://127.0.0.1:8037/traced';
    const target = new TracedWebsocketTarget(wsUrl);

    try {
      for await (const check of target.runPreflightChecks()) {
        if (!check.status.ok) {
          this.error = `${check.name}: ${check.status.error}`;
          this.connecting = false;
          m.redraw();
          return;
        }
      }
    } catch (e) {
      this.error = `Connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
      return;
    }

    this.connecting = false;
    attrs.onConnected({
      linuxTarget: target,
      deviceName: 'Linux (localhost)',
    });
    m.redraw();
  }

  private async connectWebsocket() {
    this.wsConnecting = true;
    this.error = undefined;
    this.wsDevices = [];
    m.redraw();

    const wsUrl = 'ws://127.0.0.1:8037/adb';
    using sock = await AsyncWebsocket.connect(wsUrl);
    if (!sock) {
      this.error =
        'Failed to connect to websocket_bridge at ' +
        wsUrl +
        '. Make sure websocket_bridge is running.';
      this.wsConnecting = false;
      m.redraw();
      return;
    }

    const status = await adbCmdAndWait(sock, 'host:devices-l', true);
    if (!status.ok) {
      this.error = `Failed to list devices: ${status.error}`;
      this.wsConnecting = false;
      m.redraw();
      return;
    }

    const devices: WsDevice[] = [];
    for (const line of status.value.trimEnd().split('\n')) {
      if (line === '') continue;
      const match = line.match(/^([^\s]+)\s+.*model:([^ ]+)/);
      if (!match) continue;
      devices.push({serial: match[1], model: match[2]});
    }

    this.wsDevices = devices;
    this.wsConnected = true;
    this.wsConnecting = false;
    m.redraw();
  }

  private async connectWsDevice(attrs: LandingPageAttrs, dev: WsDevice) {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    try {
      const wsUrl = 'ws://127.0.0.1:8037/adb';
      const result = await AdbWebsocketDevice.connect(
        wsUrl,
        dev.serial,
        'WEBSOCKET_BRIDGE',
      );
      if (!result.ok) {
        this.error = result.error;
        this.connecting = false;
        m.redraw();
        return;
      }

      this.connecting = false;
      attrs.onConnected({
        device: result.value,
        deviceName: `${dev.model} [${dev.serial}]`,
      });
      m.redraw();
    } catch (e) {
      this.error = `WebSocket connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
    }
  }

  private async connectWdp() {
    this.wdpConnecting = true;
    this.error = undefined;
    this.wdpDevices = [];
    m.redraw();

    const wsUrl = 'ws://127.0.0.1:9167/track-devices-json';

    for (let attempt = 0; attempt < 2; attempt++) {
      const aws = await AsyncWebsocket.connect(wsUrl);
      if (aws === undefined) {
        this.error =
          'Failed to connect to Web Device Proxy. ' +
          'Make sure it is running (see go/web-device-proxy).';
        this.wdpConnecting = false;
        m.redraw();
        return;
      }

      const respStr = await aws.waitForString();
      const respJson = JSON.parse(respStr);
      const respSchema = WDP_TRACK_DEVICES_SCHEMA.safeParse(respJson);
      if (!respSchema.success) {
        this.error = `Invalid WDP response: ${respSchema.error}`;
        this.wdpConnecting = false;
        m.redraw();
        return;
      }
      const resp = respSchema.data;

      if (
        resp.error?.type === 'ORIGIN_NOT_ALLOWLISTED' &&
        resp.error.approveUrl !== undefined
      ) {
        const popup = await showPopupWindow({url: resp.error.approveUrl});
        if (popup === false) {
          this.error = 'You need to enable popups and try again.';
          this.wdpConnecting = false;
          m.redraw();
          return;
        }
        continue; // Retry after user approved.
      } else if (resp.error !== undefined) {
        this.error = resp.error.message ?? 'Unknown WDP error';
        this.wdpConnecting = false;
        m.redraw();
        return;
      }

      // Success — listen for device updates.
      const ws = aws.release();
      this.wdpSocket = ws;
      this.wdpConnected = true;
      this.wdpConnecting = false;
      this.wdpDevices = resp.device ?? [];

      ws.onmessage = (e: MessageEvent<string>) => {
        const parsed = WDP_TRACK_DEVICES_SCHEMA.safeParse(JSON.parse(e.data));
        if (parsed.success && parsed.data.error === undefined) {
          this.wdpDevices = parsed.data.device ?? [];
        }
        m.redraw();
      };
      ws.onclose = () => {
        this.wdpConnected = false;
        this.wdpSocket = undefined;
        m.redraw();
      };
      ws.onerror = () => {
        this.wdpConnected = false;
        this.wdpSocket = undefined;
        m.redraw();
      };

      m.redraw();
      return;
    }

    this.error =
      'Failed to authenticate with WDP. ' +
      'Click allow on the popup and try again.';
    this.wdpConnecting = false;
    m.redraw();
  }

  private async connectWdpDevice(attrs: LandingPageAttrs, dev: WdpDevice) {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    try {
      if (dev.proxyStatus === 'PROXY_UNAUTHORIZED') {
        const res = await showPopupWindow({url: dev.approveUrl});
        if (!res) {
          this.error = 'Enable popups and try again.';
          this.connecting = false;
          m.redraw();
          return;
        }
      }

      if (dev.proxyStatus !== 'ADB' || dev.adbStatus !== 'DEVICE') {
        this.error =
          `Device not ready: proxyStatus=${dev.proxyStatus}` +
          ` adbStatus=${dev.adbStatus}`;
        this.connecting = false;
        m.redraw();
        return;
      }

      const wsUrl = 'ws://127.0.0.1:9167/adb-json';
      const result = await AdbWebsocketDevice.connect(
        wsUrl,
        dev.serialNumber,
        'WEB_DEVICE_PROXY',
      );
      if (!result.ok) {
        this.error = result.error;
        this.connecting = false;
        m.redraw();
        return;
      }

      const model =
        dev.proxyStatus === 'ADB' ? dev.adbProps?.model ?? '?' : '?';
      this.connecting = false;
      attrs.onConnected({
        device: result.value,
        deviceName: `${model} [${dev.serialNumber}]`,
      });
      m.redraw();
    } catch (e) {
      this.error = `WDP connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
    }
  }

  private disconnectWdp() {
    if (this.wdpSocket) {
      this.wdpSocket.close();
      this.wdpSocket = undefined;
    }
    this.wdpConnected = false;
    this.wdpDevices = [];
  }
}
