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

import {defer} from '../base/deferred';
import {Time} from '../base/time';
import {TraceFileStream} from '../core/trace_stream';

export const BUCKET_NAME = 'perfetto-ui-data';
export const MIME_JSON = 'application/json; charset=utf-8';
export const MIME_BINARY = 'application/octet-stream';

export interface GcsUploaderArgs {
  /**
   * The mime-type to use for the upload. If undefined uses
   * application/octet-stream.
   */
  mimeType?: string;

  /**
   * The name to use for the uploaded file. By default it uses a hash of
   * the passed data/blob and uses content-addressing.
   */
  fileName?: string;

  /** An optional callback that is invoked upon upload progress (or failure) */
  onProgress?: (uploader: GcsUploader) => void;
}

/**
 * A utility class to handle uploads of possibly large files to
 * Google Cloud Storage.
 * It returns immediately if the file exists already
 */
export class GcsUploader {
  state: 'UPLOADING' | 'UPLOADED' | 'ERROR' = 'UPLOADING';
  error = '';
  totalSize = 0;
  uploadedSize = 0;
  uploadedUrl = '';
  uploadedFileName = '';

  private args: GcsUploaderArgs;
  private onProgress: (_: GcsUploader) => void;
  private req: XMLHttpRequest;
  private donePromise = defer<void>();
  private startTime = performance.now();

  constructor(data: Blob | ArrayBuffer | string, args: GcsUploaderArgs) {
    this.args = args;
    this.onProgress = args.onProgress ?? ((_: GcsUploader) => {});
    this.req = new XMLHttpRequest();
    this.start(data);
  }

  async start(data: Blob | ArrayBuffer | string) {
    let fname = this.args.fileName;
    if (fname === undefined) {
      // If the file name is unspecified, hash the contents.
      if (data instanceof Blob) {
        fname = await hashFileStreaming(data);
      } else {
        fname = await sha1(data);
      }
    }
    this.uploadedFileName = fname;
    this.uploadedUrl = `https://storage.googleapis.com/${BUCKET_NAME}/${fname}`;

    // Check if the file has been uploaded already. If so, skip.
    const res = await fetch(
      `https://www.googleapis.com/storage/v1/b/${BUCKET_NAME}/o/${fname}`,
    );
    if (res.status === 200) {
      console.log(
        `Skipping upload of ${this.uploadedUrl} because it exists already`,
      );
      this.state = 'UPLOADED';
      this.donePromise.resolve();
      return;
    }

    const reqUrl =
      'https://www.googleapis.com/upload/storage/v1/b/' +
      `${BUCKET_NAME}/o?uploadType=media` +
      `&name=${fname}&predefinedAcl=publicRead`;
    this.req.onabort = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.onerror = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.upload.onprogress = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.onloadend = (e: ProgressEvent) => this.onRpcEvent(e);
    this.req.open('POST', reqUrl, /* async= */ true);
    const mimeType = this.args.mimeType ?? MIME_BINARY;
    this.req.setRequestHeader('Content-Type', mimeType);
    this.req.send(data);
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
    let str = `${Math.ceil((100 * this.uploadedSize) / this.totalSize)}%`;
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
    this.onProgress(this);
    if (done) {
      this.donePromise.resolve();
    }
  }
}

/**
 * Computes the SHA-1 of a string or ArrayBuffer(View)
 * @param data a string or ArrayBuffer to hash.
 */
async function sha1(data: string | ArrayBuffer): Promise<string> {
  let buffer: ArrayBuffer;
  if (typeof data === 'string') {
    buffer = new TextEncoder().encode(data);
  } else {
    buffer = data;
  }
  const digest = await crypto.subtle.digest('SHA-1', buffer);
  return digestToHex(digest);
}

/**
 * Converts a hash for the given file in streaming mode, without loading the
 * whole file into memory. The result is "a" SHA-1 but is not the same of
 * `shasum -a 1 file`. The reason for this is that the crypto APIs support
 * only one-shot digest computation and lack the usual update() + digest()
 * chunked API. So we end up computing a SHA-1 of the concatenation of the
 * SHA-1 of each chunk.
 * Speed: ~800 MB/s on a M2 Macbook Air 2023.
 * @param file The file to hash.
 * @returns A hex-encoded string containing the hash of the file.
 */
async function hashFileStreaming(file: Blob): Promise<string> {
  const fileStream = new TraceFileStream(file);
  let chunkDigests = '';
  for (;;) {
    const chunk = await fileStream.readChunk();
    const digest = await crypto.subtle.digest('SHA-1', chunk.data);
    chunkDigests += digestToHex(digest);
    if (chunk.eof) break;
  }
  return sha1(chunkDigests);
}

/**
 * Converts the return value of crypto.digest() to a hex string.
 * @param digest an array of bytes containing the digest
 * @returns hex-encoded string of the digest.
 */
function digestToHex(digest: ArrayBuffer): string {
  return Array.from(new Uint8Array(digest))
    .map((x) => x.toString(16).padStart(2, '0'))
    .join('');
}
