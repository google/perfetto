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

import * as uuidv4 from 'uuid/v4';
import {State} from './state';

export const PERMALINK_ID = 'permalink';
const BUCKET_NAME = 'perfetto-ui-data';

async function toSha256(str: string): Promise<string> {
  // TODO(hjd): TypeScript bug with definition of TextEncoder.
  // tslint:disable-next-line no-any
  const buffer = new (TextEncoder as any)('utf-8').encode(str);
  const digest = await crypto.subtle.digest('SHA-256', buffer);
  return Array.from(new Uint8Array(digest)).map(x => x.toString(16)).join('');
}

export async function saveState(state: State): Promise<string> {
  const text = JSON.stringify(state);
  const name = await toSha256(text);
  const url = 'https://www.googleapis.com/upload/storage/v1/b/' +
      `${BUCKET_NAME}/o?uploadType=media&name=${name}&predefinedAcl=publicRead`;
  const response = await fetch(url, {
    method: 'post',
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
    },
    body: text,
  });
  await response.json();

  return `${self.location.origin}#!/?s=${name}`;
}

export async function loadState(id: string): Promise<State> {
  const url = `https://storage.googleapis.com/${BUCKET_NAME}/${id}`;
  const response = await fetch(url);
  const text = await response.text();
  const stateHash = await toSha256(text);
  const state = JSON.parse(text);
  if (stateHash !== id) {
    throw new Error(`State hash does not match ${id} vs. ${stateHash}`);
  }
  return state;
}

export async function saveTrace(trace: File): Promise<string> {
  // TODO(hjd): This should probably also be a hash but that requires
  // trace processor support.
  const name = uuidv4();
  const url = 'https://www.googleapis.com/upload/storage/v1/b/' +
      `${BUCKET_NAME}/o?uploadType=media&name=${name}&predefinedAcl=publicRead`;
  const response = await fetch(url, {
    method: 'post',
    headers: {
      'Content-Type': 'application/octet-stream;',
    },
    body: trace,
  });
  await response.json();

  return `https://storage.googleapis.com/${BUCKET_NAME}/${name}`;
}
