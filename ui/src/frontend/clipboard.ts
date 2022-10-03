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

import {Actions} from '../common/actions';
import {QueryResponse} from '../common/queries';

import {globals} from './globals';

export function onClickCopy(url: string) {
  return (e: Event) => {
    e.preventDefault();
    copyToClipboard(url);
    globals.dispatch(Actions.updateStatus(
        {msg: 'Link copied into the clipboard', timestamp: Date.now() / 1000}));
  };
}

export async function copyToClipboard(text: string): Promise<void> {
  try {
    // TODO(hjd): Fix typescript type for navigator.
    await(navigator as any).clipboard.writeText(text);
  } catch (err) {
    console.error(`Failed to copy "${text}" to clipboard: ${err}`);
  }
}

export async function queryResponseToClipboard(resp: QueryResponse):
    Promise<void> {
  const lines: string[][] = [];
  lines.push(resp.columns);
  for (const row of resp.rows) {
    const line = [];
    for (const col of resp.columns) {
      const value = row[col];
      line.push(value === null ? 'NULL' : value.toString());
    }
    lines.push(line);
  }
  copyToClipboard(lines.map((line) => line.join('\t')).join('\n'));
}

export function download(file: File, name?: string): void {
  const url = URL.createObjectURL(file);
  const a = document.createElement('a');
  a.href = url;
  a.download = name === undefined ? file.name : name;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}


