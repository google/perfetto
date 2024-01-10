// Copyright (C) 2020 The Android Open Source Project
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

import {isString} from '../base/object_utils';
import {RecordConfig} from '../controller/record_config_types';

export const BUCKET_NAME = 'perfetto-ui-data';
import {v4 as uuidv4} from 'uuid';
import {State} from './state';
import {defer} from '../base/deferred';
import {Time} from '../base/time';

export class TraceGcsUploader {
  state: 'UPLOADING'|'UPLOADED'|'ERROR' = 'UPLOADING';
  error = '';
  totalSize = 0;
  uploadedSize = 0;
  uploadedUrl = '';
  onProgress: () => void;
  private req: XMLHttpRequest;
  private reqUrl: string;
  private donePromise = defer<void>();
  private startTime = performance.now();

  constructor(trace: File|ArrayBuffer, onProgress?: () => void) {
    // TODO(hjd): This should probably also be a hash but that requires
    // trace processor support.
    const name = uuidv4();
    this.uploadedUrl = `https://storage.googleapis.com/${BUCKET_NAME}/${name}`;
    this.reqUrl = 'https://www.googleapis.com/upload/storage/v1/b/' +
        `${BUCKET_NAME}/o?uploadType=media` +
        `&name=${name}&predefinedAcl=publicRead`;
    this.onProgress = onProgress || (() => {});
    this.req = new XMLHttpRequest();
    this.req.onabort = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.onerror = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.upload.onprogress = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.onloadend = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.open('POST', this.reqUrl);
    this.req.setRequestHeader('Content-Type', 'application/octet-stream');
    this.req.send(trace);
  }

  waitForCompletion(): Promise<void> {
    return this.donePromise;
  }

  abort() {
    if (this.state === 'UPLOADING') {
      this.req.abort();
    }
  }

  getEtaString() {
    let str = `${Math.ceil(100 * this.uploadedSize / this.totalSize)}%`;
    str += ` (${(this.uploadedSize / 1e6).toFixed(2)} MB)`;
    const elapsed = (performance.now() - this.startTime) / 1000;
    const rate = this.uploadedSize / elapsed;
    const etaSecs = Math.round((this.totalSize - this.uploadedSize) / rate);
    str += ' - ETA: ' + Time.toTimecode(Time.fromSeconds(etaSecs)).dhhmmss;
    return str;
  }

  private onRpcEvent(e: ProgressEvent) {
    let done = false;
    switch (e.type) {
      case 'progress':
        this.uploadedSize = e.loaded;
        this.totalSize = e.total;
        break;
      case 'abort':
        this.state = 'ERROR';
        this.error = 'Upload aborted';
        break;
      case 'error':
        this.state = 'ERROR';
        this.error = `${this.req.status} - ${this.req.statusText}`;
        break;
      case 'loadend':
        done = true;
        if (this.req.status === 200) {
          this.state = 'UPLOADED';
        } else if (this.state === 'UPLOADING') {
          this.state = 'ERROR';
          this.error = `${this.req.status} - ${this.req.statusText}`;
        }
        break;
      default:
        return;
    }
    this.onProgress();
    if (done) {
      this.donePromise.resolve();
    }
  }
}

// Bigint's are not serializable using JSON.stringify, so we use a special
// object when serialising
export type SerializedBigint = {
  __kind: 'bigint',
  value: string
};

// Check if a value looks like a serialized bigint
export function isSerializedBigint(value: unknown): value is SerializedBigint {
  if (value === null) {
    return false;
  }
  if (typeof value !== 'object') {
    return false;
  }
  if ('__kind' in value && 'value' in value) {
    return value.__kind === 'bigint' && isString(value.value);
  }
  return false;
}

export function serializeStateObject(object: unknown): string {
  const json = JSON.stringify(object, (key, value) => {
    if (typeof value === 'bigint') {
      return {
        __kind: 'bigint',
        value: value.toString(),
      };
    }
    return key === 'nonSerializableState' ? undefined : value;
  });
  return json;
}

export function deserializeStateObject<T>(json: string): T {
  const object = JSON.parse(json, (_key, value) => {
    if (isSerializedBigint(value)) {
      return BigInt(value.value);
    }
    return value;
  });
  return object as T;
}

export async function saveState(stateOrConfig: State|
                                RecordConfig): Promise<string> {
  const text = serializeStateObject(stateOrConfig);
  const hash = await toSha256(text);
  const url = 'https://www.googleapis.com/upload/storage/v1/b/' +
      `${BUCKET_NAME}/o?uploadType=media` +
      `&name=${hash}&predefinedAcl=publicRead`;
  const response = await fetch(url, {
    method: 'post',
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
    },
    body: text,
  });
  await response.json();
  return hash;
}

// This has a bug:
// x.toString(16) doesn't zero pad so if the digest is:
// [23, 7, 42, ...]
// You get:
// ['17', '7', '2a', ...] = 1772a...
// Rather than:
// ['17', '07', '2a', ...] = 17072a...
// As you ought to (and as the hexdigest is computed by e.g. Python).
// Unfortunately there are a lot of old permalinks out there so we
// still need this broken implementation to check their hashes.
export async function buggyToSha256(str: string): Promise<string> {
  const buffer = new TextEncoder().encode(str);
  const digest = await crypto.subtle.digest('SHA-256', buffer);
  return Array.from(new Uint8Array(digest)).map((x) => x.toString(16)).join('');
}

export async function toSha256(str: string): Promise<string> {
  const buffer = new TextEncoder().encode(str);
  const digest = await crypto.subtle.digest('SHA-256', buffer);
  return Array.from(new Uint8Array(digest))
      .map((x) => x.toString(16).padStart(2, '0'))
      .join('');
}
