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

import {QuerySlot, SerialTaskQueue, QUERY_CANCELLED} from './query_slot';

// Helper to wait for pending promises to resolve
const flushPromises = () => new Promise((resolve) => setTimeout(resolve, 0));

test('basic query execution', async () => {
  const queue = new SerialTaskQueue();
  const slot = new QuerySlot<number>(queue);

  const queryFn = jest.fn().mockImplementation(async () => 42);

  const result1 = slot.use({
    key: {id: 1},
    queryFn,
  });

  expect(result1.data).toBeUndefined();
  expect(result1.isPending).toBe(true);

  await flushPromises();

  const result2 = slot.use({
    key: {id: 1},
    queryFn,
  });

  expect(result2.data).toBe(42);
  expect(result2.isPending).toBe(false);
  expect(queryFn).toHaveBeenCalledTimes(1);
});

test('cached result is returned without re-query', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  const queryFn = jest.fn().mockResolvedValue(42);

  slot.use({key: {id: 1}, queryFn});
  await flushPromises();

  // Call use() again with same key - should return cached, not re-query
  const result = slot.use({key: {id: 1}, queryFn});

  expect(result.data).toBe(42);
  expect(result.isFresh).toBe(true);
  expect(queryFn).toHaveBeenCalledTimes(1); // Still just once
});

test('dispose cancels queued work across render cycles', async () => {
  const executor = new SerialTaskQueue();

  // Simulate first component mounting and starting a long query
  const slot1 = new QuerySlot<number>(executor);
  let query1Resolved = false;
  const queryFn1 = jest.fn().mockImplementation(async () => {
    await flushPromises(); // Simulate some async work
    query1Resolved = true;
    return 1;
  });
  slot1.use({key: {id: 1}, queryFn: queryFn1});

  // Simulate second component mounting while first query is in-flight
  const slot2 = new QuerySlot<number>(executor);
  const queryFn2 = jest.fn().mockResolvedValue(2);
  slot2.use({key: {id: 2}, queryFn: queryFn2});

  // Second component unmounts before its query runs (it's queued behind first)
  slot2.dispose();

  // Wait for first query to complete
  await flushPromises();
  await flushPromises();

  expect(queryFn1).toHaveBeenCalledTimes(1);
  expect(query1Resolved).toBe(true);
  // Second query was cancelled because slot2 was disposed while queued
  expect(queryFn2).not.toHaveBeenCalled();
});

test('multiple slots same executor run serially', async () => {
  const executor = new SerialTaskQueue();
  const slot1 = new QuerySlot<number>(executor);
  const slot2 = new QuerySlot<number>(executor);

  const order: number[] = [];

  const queryFn1 = jest.fn().mockImplementation(async () => {
    order.push(1);
    return 1;
  });

  const queryFn2 = jest.fn().mockImplementation(async () => {
    order.push(2);
    return 2;
  });

  slot1.use({key: {id: 1}, queryFn: queryFn1});
  slot2.use({key: {id: 2}, queryFn: queryFn2});

  await flushPromises();
  await flushPromises();

  expect(queryFn1).toHaveBeenCalledTimes(1);
  expect(queryFn2).toHaveBeenCalledTimes(1);
  expect(order).toEqual([1, 2]);
});

test('rapid key changes on same slot only runs first and last', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  const queryFn1 = jest.fn().mockResolvedValue(1);
  const queryFn2 = jest.fn().mockResolvedValue(2);
  const queryFn3 = jest.fn().mockResolvedValue(3);

  // Schedule three different queries rapidly on the same slot
  // First one starts immediately, subsequent ones replace in pending queue
  slot.use({key: {id: 1}, queryFn: queryFn1});
  slot.use({key: {id: 2}, queryFn: queryFn2});
  slot.use({key: {id: 3}, queryFn: queryFn3});

  await flushPromises();
  await flushPromises();

  // First query runs immediately, intermediate is skipped, last runs after
  expect(queryFn1).toHaveBeenCalledTimes(1);
  expect(queryFn2).not.toHaveBeenCalled();
  expect(queryFn3).toHaveBeenCalledTimes(1);
});

test('retainOn allows showing previous data during compatible changes', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  const queryFn1 = jest.fn().mockResolvedValue(100);

  // First query
  slot.use({
    key: {filter: 'a', page: 1},
    queryFn: queryFn1,
    retainOn: ['page'],
  });

  await flushPromises();

  // Verify first result cached
  const result1 = slot.use({
    key: {filter: 'a', page: 1},
    queryFn: queryFn1,
    retainOn: ['page'],
  });
  expect(result1.data).toBe(100);
  expect(result1.isFresh).toBe(true);

  // Change only page (in retainOn) - should show stale data
  const queryFn2 = jest.fn().mockResolvedValue(200);
  const result2 = slot.use({
    key: {filter: 'a', page: 2},
    queryFn: queryFn2,
    retainOn: ['page'],
  });

  expect(result2.data).toBe(100); // Stale data shown
  expect(result2.isFresh).toBe(false);
  expect(result2.isPending).toBe(true);

  // Change filter (not in retainOn) - should NOT show stale data
  const queryFn3 = jest.fn().mockResolvedValue(300);
  const result3 = slot.use({
    key: {filter: 'b', page: 2},
    queryFn: queryFn3,
    retainOn: ['page'],
  });

  expect(result3.data).toBeUndefined(); // No stale data
  expect(result3.isPending).toBe(true);
});

test('enabled prevents query from running', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  const queryFn = jest.fn().mockResolvedValue(42);

  // Query with enabled=false
  slot.use({
    key: {id: 1},
    queryFn,
    enabled: false,
  });

  await flushPromises();

  expect(queryFn).not.toHaveBeenCalled();

  // Now enable it
  slot.use({
    key: {id: 1},
    queryFn,
    enabled: true,
  });

  await flushPromises();

  const result = slot.use({
    key: {id: 1},
    queryFn,
    enabled: true,
  });

  expect(result.data).toBe(42);
  expect(queryFn).toHaveBeenCalledTimes(1);
});

test('use after dispose throws', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  const queryFn = jest.fn().mockResolvedValue(42);

  slot.use({key: {id: 1}, queryFn});
  await flushPromises();

  slot.dispose();

  expect(() => slot.use({key: {id: 1}, queryFn})).toThrow(
    'QuerySlot.use() called after dispose()',
  );
});

test('queued work is cancelled when slot is disposed', async () => {
  const executor = new SerialTaskQueue();
  const slot1 = new QuerySlot<number>(executor);
  const slot2 = new QuerySlot<number>(executor);

  // Start a slow query on slot1
  const queryFn1 = jest.fn().mockImplementation(async () => {
    await flushPromises();
    return 1;
  });
  slot1.use({key: {id: 1}, queryFn: queryFn1});

  // Queue work on slot2
  const queryFn2 = jest.fn().mockResolvedValue(2);
  slot2.use({key: {id: 2}, queryFn: queryFn2});

  // Dispose slot2 while its work is still queued
  slot2.dispose();

  // Let everything complete
  await flushPromises();
  await flushPromises();

  expect(queryFn1).toHaveBeenCalledTimes(1);
  expect(queryFn2).not.toHaveBeenCalled();
});

test('AsyncDisposable is disposed before running next queryFn', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<AsyncDisposable>(executor);

  const events: string[] = [];

  const disposable1: AsyncDisposable = {
    [Symbol.asyncDispose]: jest.fn().mockImplementation(async () => {
      events.push('dispose1');
    }),
  };

  const disposable2: AsyncDisposable = {
    [Symbol.asyncDispose]: jest.fn().mockImplementation(async () => {
      events.push('dispose2');
    }),
  };

  const queryFn1 = jest.fn().mockImplementation(async () => {
    events.push('query1');
    return disposable1;
  });

  const queryFn2 = jest.fn().mockImplementation(async () => {
    events.push('query2');
    return disposable2;
  });

  // First query
  slot.use({key: {id: 1}, queryFn: queryFn1});
  await flushPromises();

  expect(events).toEqual(['query1']);

  // Second query with different key - should dispose first before running second
  slot.use({key: {id: 2}, queryFn: queryFn2});
  await flushPromises();

  expect(events).toEqual(['query1', 'dispose1', 'query2']);
  expect(disposable1[Symbol.asyncDispose]).toHaveBeenCalledTimes(1);
});

test('slot dispose calls AsyncDisposable dispose after in-flight task completes', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<AsyncDisposable>(executor);

  const events: string[] = [];
  let resolveQuery: () => void;
  const queryPromise = new Promise<void>((resolve) => {
    resolveQuery = resolve;
  });

  const disposable: AsyncDisposable = {
    [Symbol.asyncDispose]: jest.fn().mockImplementation(async () => {
      events.push('disposed');
    }),
  };

  const queryFn = jest.fn().mockImplementation(async () => {
    events.push('query-start');
    await queryPromise;
    events.push('query-end');
    return disposable;
  });

  // Start query
  slot.use({key: {id: 1}, queryFn});
  await flushPromises();

  expect(events).toEqual(['query-start']);

  // Dispose slot while query is in-flight
  slot.dispose();
  await flushPromises();

  // Dispose should not have been called yet - query is still running
  expect(events).toEqual(['query-start']);
  expect(disposable[Symbol.asyncDispose]).not.toHaveBeenCalled();

  // Complete the query
  resolveQuery!();
  await flushPromises();
  await flushPromises();

  // Now the dispose should have been scheduled and run
  expect(events).toEqual(['query-start', 'query-end', 'disposed']);
  expect(disposable[Symbol.asyncDispose]).toHaveBeenCalledTimes(1);
});

test('cancellation signal is set when new query is scheduled', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  const events: string[] = [];
  let query1Signal: {isCancelled: boolean} | undefined;
  let resolveQuery1: () => void;
  const query1Promise = new Promise<void>((resolve) => {
    resolveQuery1 = resolve;
  });

  const queryFn1 = jest.fn().mockImplementation(async (signal) => {
    query1Signal = signal;
    events.push('query1-start');
    await query1Promise;
    events.push(`query1-end (cancelled=${signal.isCancelled})`);
    if (Boolean(signal.isCancelled)) {
      return QUERY_CANCELLED;
    }
    return 1;
  });

  const queryFn2 = jest.fn().mockImplementation(async (signal) => {
    events.push(`query2 (cancelled=${signal.isCancelled})`);
    return 2;
  });

  // Start first query
  slot.use({key: {id: 1}, queryFn: queryFn1});
  await flushPromises();

  expect(events).toEqual(['query1-start']);
  expect(query1Signal?.isCancelled).toBe(false);

  // Schedule second query while first is in-flight
  slot.use({key: {id: 2}, queryFn: queryFn2});

  // First query's signal should now be cancelled
  expect(query1Signal?.isCancelled).toBe(true);

  // Complete first query
  resolveQuery1!();
  await flushPromises();
  await flushPromises();

  // First query returned QUERY_CANCELLED so it shouldn't be cached
  // Second query should have run
  expect(events).toEqual([
    'query1-start',
    'query1-end (cancelled=true)',
    'query2 (cancelled=false)',
  ]);

  // Final result should be from query2
  const result = slot.use({key: {id: 2}, queryFn: queryFn2});
  expect(result.data).toBe(2);
});

test('QUERY_CANCELLED result is not cached', async () => {
  const executor = new SerialTaskQueue();
  const slot = new QuerySlot<number>(executor);

  // Query that always returns QUERY_CANCELLED
  const queryFn = jest.fn().mockImplementation(async () => QUERY_CANCELLED);

  slot.use({key: {id: 1}, queryFn});
  await flushPromises();

  // Result should be undefined since QUERY_CANCELLED was returned
  const result = slot.use({key: {id: 1}, queryFn});
  expect(result.data).toBeUndefined();

  // Wait for the second query to execute
  await flushPromises();

  // Query should be called again since nothing was cached
  expect(queryFn).toHaveBeenCalledTimes(2);
});
