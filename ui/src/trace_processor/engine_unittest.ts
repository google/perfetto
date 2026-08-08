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

import {describe, expect, test} from 'vitest';
import protos from '../protos';
import {EngineBase} from './engine';

const TPM = protos.TraceProcessorRpc.TraceProcessorMethod;

class FakeEngine extends EngineBase {
  readonly id = 'fake';
  readonly mode = 'WASM';
  readonly requests: Uint8Array[] = [];

  rpcSendRequestBytes(data: Uint8Array): void {
    this.requests.push(data);
  }

  completeQuery(): void {
    const response = new protos.TraceProcessorRpc({
      seq: 0,
      response: TPM.TPM_QUERY_STREAMING,
      queryResult: new protos.QueryResult({
        batch: [new protos.QueryResult.CellsBatch({isLastBatch: true})],
      }),
    });
    const stream = new protos.TraceProcessorRpcStream({msg: [response]});
    this.onRpcResponseBytes(
      protos.TraceProcessorRpcStream.encode(stream).finish(),
    );
  }

  [Symbol.dispose](): void {}
}

describe('Engine query sequences', () => {
  test('a cancellable tail blocks only its exact tag', () => {
    const engine = new FakeEngine();
    engine.query('SELECT 1', {tag: 'foo', cancellable: true});

    expect(() => engine.query('SELECT 2', {tag: 'foo'})).toThrow(
      /cancellable tail query is outstanding/,
    );
    expect(() => engine.query('SELECT 3', {tag: 'foo/bar'})).not.toThrow();
    engine.completeQuery();
    engine.completeQuery();
  });

  test('fallback cancellation waits for the terminal response', async () => {
    const engine = new FakeEngine();
    const query = engine.query('SELECT 1', {
      tag: 'fallback',
      cancellable: true,
    });
    let cancelResolved = false;
    const cancellation = query.cancel().then(() => {
      cancelResolved = true;
    });

    await Promise.resolve();
    expect(cancelResolved).toBe(false);
    engine.completeQuery();
    await cancellation;
    await expect(query).resolves.toBeDefined();
  });
});
