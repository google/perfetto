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
import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';

// jsbn-rsa calls the Netscape-4-era window.crypto.random() under jsdom's
// default navigator.appVersion; mock it since this test does not run RSA math.
vi.mock('jsbn-rsa', () => ({
  BigInteger: {ONE: {shiftLeft: () => ({})}},
  RSAKey: class {},
}));

import {okResult} from '../../../base/result';
import type {App} from '../../../public/app';
import {AdbKeyManager} from '../../dev.perfetto.RecordTraceV2/adb/webusb/adb_key_manager';
import {AdbWebusbDevice} from '../../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_device';
import {ADB_DEVICE_FILTER} from '../../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_utils';
import {ConnectionPage, type ConnectionResult} from './connection';

function fakeApp(): App {
  return {
    policy: {bugReportUrl: 'https://example.com/bug'},
  } as unknown as App;
}

function fakeUsbDevice(serial: string, productName: string): USBDevice {
  return {
    vendorId: 0x18d1,
    productId: 0x4ee7,
    deviceVersionMajor: 1,
    deviceVersionMinor: 0,
    serialNumber: serial,
    productName,
    configurations: [
      {
        configurationValue: 1,
        interfaces: [
          {
            interfaceNumber: 0,
            alternates: [
              {
                interfaceClass: ADB_DEVICE_FILTER.classCode,
                interfaceSubclass: ADB_DEVICE_FILTER.subclassCode,
                interfaceProtocol: ADB_DEVICE_FILTER.protocolCode,
                endpoints: [
                  {
                    type: 'bulk',
                    direction: 'in',
                    endpointNumber: 1,
                    packetSize: 512,
                  },
                  {
                    type: 'bulk',
                    direction: 'out',
                    endpointNumber: 2,
                    packetSize: 512,
                  },
                ],
              },
            ],
          },
        ],
      },
    ],
  } as unknown as USBDevice;
}

class FakeUsb {
  devices: USBDevice[] = [];
  readonly requestDevice = vi.fn<() => Promise<USBDevice>>();
  readonly getDevices = vi.fn(async () => [...this.devices]);
  private readonly listeners = new Map<string, Set<EventListener>>();

  addEventListener(type: string, cb: EventListener) {
    let set = this.listeners.get(type);
    if (!set) {
      set = new Set();
      this.listeners.set(type, set);
    }
    set.add(cb);
  }

  removeEventListener(type: string, cb: EventListener) {
    this.listeners.get(type)?.delete(cb);
  }

  listenerCount(type: string): number {
    return this.listeners.get(type)?.size ?? 0;
  }

  emit(type: string) {
    for (const cb of this.listeners.get(type) ?? []) {
      cb(new Event(type));
    }
  }
}

async function flush() {
  await Promise.resolve();
  await Promise.resolve();
  m.redraw.sync();
}

describe('ConnectionPage WebUSB', () => {
  let container: HTMLElement;
  let fakeUsb: FakeUsb;
  let origUsbDesc: PropertyDescriptor | undefined;

  beforeEach(() => {
    container = document.createElement('div');
    document.body.appendChild(container);
    fakeUsb = new FakeUsb();
    origUsbDesc = Object.getOwnPropertyDescriptor(navigator, 'usb');
    Object.defineProperty(navigator, 'usb', {
      value: fakeUsb,
      configurable: true,
    });
  });

  afterEach(() => {
    m.mount(container, null);
    container.remove();
    vi.restoreAllMocks();
    if (origUsbDesc) {
      Object.defineProperty(navigator, 'usb', origUsbDesc);
    } else {
      delete (navigator as unknown as {usb?: unknown}).usb;
    }
  });

  test('shares a single AdbKeyManager instance across callers', () => {
    expect(AdbKeyManager.getShared()).toBe(AdbKeyManager.getShared());
  });

  test('lists paired USB devices and reconnects without calling requestDevice', async () => {
    const dev = fakeUsbDevice('SER123', 'Pixel 9');
    fakeUsb.devices = [dev];

    const fakeAdb = {close: vi.fn()} as unknown as AdbWebusbDevice;
    const connectSpy = vi
      .spyOn(AdbWebusbDevice, 'connect')
      .mockResolvedValue(okResult(fakeAdb));

    let connected: ConnectionResult | undefined;
    m.mount(container, {
      view: () =>
        m(ConnectionPage, {
          app: fakeApp(),
          onConnected: (r) => {
            connected = r;
          },
        }),
    });
    await flush();

    expect(fakeUsb.getDevices).toHaveBeenCalledTimes(1);
    const buttons = Array.from(container.querySelectorAll('button'));
    const devBtn = buttons.find((b) =>
      b.textContent?.includes('Pixel 9 [SER123]'),
    );
    expect(devBtn).toBeDefined();
    expect(
      buttons.some((b) => b.textContent?.includes('Pair new USB device')),
    ).toBe(true);

    devBtn!.click();
    await flush();

    expect(fakeUsb.requestDevice).not.toHaveBeenCalled();
    expect(connectSpy).toHaveBeenCalledWith(dev, AdbKeyManager.getShared());
    expect(connected?.device).toBe(fakeAdb);
    expect(connected?.deviceName).toBe('Pixel 9 [SER123]');
  });

  test('refreshes paired devices on USB hotplug and removes listeners on unmount', async () => {
    m.mount(container, {
      view: () =>
        m(ConnectionPage, {
          app: fakeApp(),
          onConnected: () => {},
        }),
    });
    await flush();

    expect(fakeUsb.listenerCount('connect')).toBe(1);
    expect(fakeUsb.listenerCount('disconnect')).toBe(1);
    expect(container.textContent).toContain('Connect USB device');

    fakeUsb.devices = [fakeUsbDevice('HOT1', 'Pixel 8')];
    fakeUsb.emit('connect');
    await flush();
    expect(container.textContent).toContain('Pixel 8 [HOT1]');

    fakeUsb.devices = [];
    fakeUsb.emit('disconnect');
    await flush();
    expect(container.textContent).not.toContain('Pixel 8 [HOT1]');

    m.mount(container, null);
    expect(fakeUsb.listenerCount('connect')).toBe(0);
    expect(fakeUsb.listenerCount('disconnect')).toBe(0);
  });

  test('pairs a new USB device via requestDevice and keeps shared AdbKeyManager across remounts', async () => {
    const dev = fakeUsbDevice('NEW1', 'Pixel 10');
    fakeUsb.requestDevice.mockImplementation(async () => {
      fakeUsb.devices = [dev];
      return dev;
    });

    const fakeAdb = {close: vi.fn()} as unknown as AdbWebusbDevice;
    const connectSpy = vi
      .spyOn(AdbWebusbDevice, 'connect')
      .mockResolvedValue(okResult(fakeAdb));

    m.mount(container, {
      view: () => m(ConnectionPage, {app: fakeApp(), onConnected: () => {}}),
    });
    await flush();

    const pairBtn = Array.from(container.querySelectorAll('button')).find((b) =>
      b.textContent?.includes('Connect USB device'),
    );
    expect(pairBtn).toBeDefined();
    pairBtn!.click();
    await flush();

    expect(fakeUsb.requestDevice).toHaveBeenCalledTimes(1);
    const firstKeyMgr = connectSpy.mock.calls[0]?.[1];
    expect(firstKeyMgr).toBe(AdbKeyManager.getShared());

    // Unmount and remount ConnectionPage (simulating connect -> disconnect):
    // paired device should now appear directly without requestDevice and reuse
    // the exact same AdbKeyManager instance.
    m.mount(container, null);
    m.mount(container, {
      view: () => m(ConnectionPage, {app: fakeApp(), onConnected: () => {}}),
    });
    await flush();

    const devBtn = Array.from(container.querySelectorAll('button')).find((b) =>
      b.textContent?.includes('Pixel 10 [NEW1]'),
    );
    expect(devBtn).toBeDefined();
    devBtn!.click();
    await flush();

    expect(fakeUsb.requestDevice).toHaveBeenCalledTimes(1);
    expect(connectSpy).toHaveBeenCalledTimes(2);
    expect(connectSpy.mock.calls[1]?.[1]).toBe(firstKeyMgr);
  });
});
