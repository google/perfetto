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

import type {Transport} from '@codemirror/lsp-client';
import type {Engine} from 'syntaqlite';

// Bridges @codemirror/lsp-client to the LSP server running in-process inside
// the syntaqlite WASM engine. send() is a direct function call; the server's
// replies are delivered on a microtask so the client's async expectations
// hold.
export function engineTransport(engine: Engine): Transport {
  const handlers: Array<(value: string) => void> = [];
  return {
    send(message: string): void {
      const out = engine.lspMessage(message);
      if (out.length === 0) return;
      queueMicrotask(() => {
        for (const msg of out) {
          const text = JSON.stringify(msg);
          for (const handler of [...handlers]) handler(text);
        }
      });
    },
    subscribe(handler: (value: string) => void): void {
      handlers.push(handler);
    },
    unsubscribe(handler: (value: string) => void): void {
      const i = handlers.indexOf(handler);
      if (i >= 0) handlers.splice(i, 1);
    },
  };
}
