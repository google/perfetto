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
import protos from '../../../protos';
import {defer, Deferred} from '../../../base/deferred';
import {errResult, okResult, Result} from '../../../base/result';
import {binaryEncode} from '../../../base/string_utils';
import {exists} from '../../../base/utils';
import {PreflightCheck} from '../interfaces/connection_check';
import {RecordingTarget} from '../interfaces/recording_target';
import {TargetPlatformId} from '../interfaces/target_platform';
import {ChromeExtensionTracingSession} from './chrome_extension_tracing_session';

const EXTENSION_ID = 'lfmkphfpdbjijhpomgecfikhfohaoine';
const EXTENSION_URL = `https://g.co/chrome/tracing-extension`;

export class ChromeExtensionTarget implements RecordingTarget {
  readonly id = 'chrome_extension';
  readonly kind = 'LIVE_RECORDING';
  readonly transportType = 'Extension';
  platform: TargetPlatformId = 'CHROME';
  private port?: chrome.runtime.Port;
  private _connected = false;
  private _extensionVersion?: string;
  private _connectPromise?: Deferred<boolean>;
  private chromeCategories?: string[];
  private chromeCategoriesPromise = defer<string[]>();
  private session?: ChromeExtensionTracingSession;

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    yield {
      name: 'Tracing Extension',
      status: await (async (): Promise<Result<string>> => {
        const err = errResult(`Not found. Please install ${EXTENSION_URL}`);
        if (!exists(window.chrome) || !exists(window.chrome.runtime)) {
          return err;
        }
        await this.connectIfNeeded();
        return this._connected
          ? okResult(`Connected (version: ${this._extensionVersion})`)
          : err;
      })(),
    };

    if (this.platform === 'CHROME_OS') {
      yield {
        name: 'CrOS detection',
        status: ((): Result<string> => {
          const userAgent = navigator.userAgent;
          const isChromeOS = /CrOS/.test(userAgent);
          return isChromeOS ? okResult(userAgent) : errResult(userAgent);
        })(),
      };
    }
  }

  async connectIfNeeded(): Promise<boolean> {
    if (!exists(window.chrome) || !exists(window.chrome.runtime)) {
      return false;
    }
    if (this._connected) return true;
    this.port = window.chrome.runtime.connect(EXTENSION_ID);
    this.port.onMessage.addListener(this.onExtensionMessage.bind(this));
    this.port.onDisconnect.addListener(this.onExtensionDisconnect.bind(this));

    // This promise is resolved once the extension replies with 'version'.
    // Unfortunately the chrome.runtime API doesn't offer a way to tell if the
    // extension exists or not. The port is always connected. If the extension
    // doesn't exist, then we receive an onDisconnect soon after.
    const retPromise = defer<boolean>();
    this._connectPromise = retPromise;

    // This will trigger a promise resolution once the extension replies with
    // the version (in onExtensionMessage() below);
    this.invokeExtensionMethod('ExtensionVersion');
    return retPromise;
  }

  disconnect(): void {
    this._connected = false;
    this.port?.disconnect();
    this.port = undefined;
  }

  get connected(): boolean {
    return this._connected;
  }

  get name(): string {
    return 'Chrome (this browser)';
  }

  get emitsCompressedtrace(): boolean {
    return this.platform === 'CHROME';
  }

  async getServiceState(): Promise<Result<protos.ITracingServiceState>> {
    const categories = await this.getChromeCategories();
    if (!categories.ok) return categories;
    return okResult(categoriesToServiceState(categories.value));
  }

  async getChromeCategories(): Promise<Result<string[]>> {
    if (this.chromeCategories === undefined) {
      if (!(await this.connectIfNeeded())) {
        return errResult('Tracing extension not detected');
      }
      this.chromeCategories = await this.chromeCategoriesPromise;
    }
    return okResult(this.chromeCategories);
  }

  async startTracing(
    traceConfig: protos.ITraceConfig,
  ): Promise<Result<ChromeExtensionTracingSession>> {
    await this.connectIfNeeded();
    if (!this._connected) {
      return errResult('Cannot connect to the Chrome Tracing extension');
    }
    this.session = new ChromeExtensionTracingSession(this, traceConfig);
    return okResult(this.session);
  }

  private onExtensionMessage(msg: object): void {
    if ('version' in msg) {
      this._connected = true;
      this._extensionVersion = `${msg.version}`;
      const cp = this._connectPromise;
      this._connectPromise = undefined;
      cp?.resolve(true);
      this.invokeExtensionMethod('GetCategories');
      return;
    }

    if (!('type' in msg)) {
      return;
    }

    if (msg.type === 'GetCategoriesResponse') {
      const cats = (msg as {type: string; categories: string[]}).categories;
      this.chromeCategoriesPromise.resolve(cats);
    } else {
      this.session?.onExtensionMessage(`${msg.type}`, msg);
    }
  }

  invokeExtensionMethod(method: string, data?: Uint8Array) {
    const requestData = binaryEncode(data ?? new Uint8Array());
    this.port?.postMessage({method, requestData});
  }

  private onExtensionDisconnect() {
    if (this._connected) {
      console.log(
        'Chrome tracing extension disconnected',
        chrome.runtime.lastError,
      );
    }
    void chrome.runtime.lastError;
    this.port = undefined;
    this._connected = false;
    if (this._connectPromise) {
      this._connectPromise.resolve(false);
    }
    m.redraw();
  }
}

function categoriesToServiceState(
  categories: string[],
): protos.ITracingServiceState {
  return {
    producers: [{id: 1, name: 'Chrome'}],
    dataSources: [
      {
        producerId: 1,
        dsDescriptor: {
          name: 'track_event',
          id: 1,
          trackEventDescriptor: {
            availableCategories: categories.map((cat) => ({name: cat})),
          },
        },
      },
    ],
  };
}
