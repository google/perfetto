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

import {QueryStore} from './query_store';

describe('QueryStore.getOrCreate', () => {
  test('creates a new entry with sane defaults', () => {
    const store = new QueryStore();
    const exec = store.getOrCreate('uuid-1');
    expect(exec.uuid).toBe('uuid-1');
    expect(exec.status).toBe('UNKNOWN');
    expect(exec.processedRows).toBe(0);
    expect(exec.processedTraces).toBe(0);
    expect(exec.totalTraces).toBe(0);
  });

  test('returns the same instance on subsequent calls (identity)', () => {
    const store = new QueryStore();
    const a = store.getOrCreate('uuid-1');
    const b = store.getOrCreate('uuid-1');
    expect(b).toBe(a);
  });

  test('initialData on first call seeds the entry', () => {
    const store = new QueryStore();
    const exec = store.getOrCreate('uuid-1', {
      materialized: true,
      perfettoSql: 'SELECT 1',
    });
    expect(exec.materialized).toBe(true);
    expect(exec.perfettoSql).toBe('SELECT 1');
  });
});

describe('QueryStore merge rules (getOrCreate on existing entry)', () => {
  test('overwrites everything when stored entry is terminal', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {
      status: 'SUCCESS',
      processedRows: 100,
    });
    // Terminal entry trusts the incoming snapshot.
    store.getOrCreate('uuid-1', {
      status: 'CANCELLED',
      processedRows: 50,
    });
    const exec = store.getOrCreate('uuid-1');
    expect(exec.status).toBe('CANCELLED');
    expect(exec.processedRows).toBe(50);
  });

  test('overwrites everything when incoming snapshot is terminal', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {
      status: 'IN_PROGRESS',
      processedRows: 10,
    });
    // Live entry, but the incoming snapshot is terminal — accept it.
    store.getOrCreate('uuid-1', {
      status: 'SUCCESS',
      processedRows: 8,
      endTime: 1000,
    });
    const exec = store.getOrCreate('uuid-1');
    expect(exec.status).toBe('SUCCESS');
    expect(exec.processedRows).toBe(8);
    expect(exec.endTime).toBe(1000);
  });

  test('overwrites when incoming row count is at least as high', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {status: 'IN_PROGRESS', processedRows: 10});
    store.getOrCreate('uuid-1', {status: 'IN_PROGRESS', processedRows: 25});
    const exec = store.getOrCreate('uuid-1');
    expect(exec.processedRows).toBe(25);
  });

  test('preserves live progress when incoming snapshot is staler', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {
      status: 'IN_PROGRESS',
      processedRows: 50,
      processedTraces: 12,
    });
    // History list arrives with old data; should NOT regress counters.
    store.getOrCreate('uuid-1', {
      status: 'IN_PROGRESS',
      processedRows: 10,
      processedTraces: 2,
      tableLink: '/t/abc',
      perfettoSql: 'SELECT 2',
    });
    const exec = store.getOrCreate('uuid-1');
    expect(exec.processedRows).toBe(50);
    expect(exec.processedTraces).toBe(12);
    // Static metadata still gets carried over.
    expect(exec.tableLink).toBe('/t/abc');
    expect(exec.perfettoSql).toBe('SELECT 2');
  });

  test('UNKNOWN status counts as live', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {status: 'UNKNOWN', processedRows: 10});
    store.getOrCreate('uuid-1', {status: 'UNKNOWN', processedRows: 5});
    const exec = store.getOrCreate('uuid-1');
    expect(exec.processedRows).toBe(10);
  });
});

describe('QueryStore.update', () => {
  test('partial updates preserve unrelated fields', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {
      status: 'IN_PROGRESS',
      processedRows: 10,
      perfettoSql: 'SELECT 1',
    });
    store.update('uuid-1', {processedRows: 20});
    const exec = store.getOrCreate('uuid-1');
    expect(exec.processedRows).toBe(20);
    expect(exec.perfettoSql).toBe('SELECT 1');
    expect(exec.status).toBe('IN_PROGRESS');
  });

  test('update on a missing entry is a no-op', () => {
    const store = new QueryStore();
    expect(() =>
      store.update('does-not-exist', {processedRows: 1}),
    ).not.toThrow();
    expect(store.getAll()).toHaveLength(0);
  });
});

describe('QueryStore truncation merge', () => {
  // Listing clips perfettoSql/error; merge must not downgrade longer→shorter.

  test('shorter (truncated) SQL does not overwrite existing full SQL', () => {
    const store = new QueryStore();
    const fullSql = 'SELECT * FROM slice WHERE name = "verbose..."';
    store.getOrCreate('uuid-1', {status: 'SUCCESS', perfettoSql: fullSql});
    store.getOrCreate('uuid-1', {
      status: 'SUCCESS',
      perfettoSql: 'SELECT * FROM slic…',
    });
    expect(store.getOrCreate('uuid-1').perfettoSql).toBe(fullSql);
  });

  test('truncated SQL fills an empty slot', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {status: 'SUCCESS'});
    store.getOrCreate('uuid-1', {perfettoSql: 'SELECT * FROM slic…'});
    expect(store.getOrCreate('uuid-1').perfettoSql).toBe('SELECT * FROM slic…');
  });

  test('longer (full) SQL upgrades a previously-truncated entry', () => {
    const store = new QueryStore();
    store.getOrCreate('uuid-1', {
      status: 'SUCCESS',
      perfettoSql: 'SELECT * FROM slic…',
    });
    const fullSql = 'SELECT * FROM slice WHERE name = "verbose..."';
    store.getOrCreate('uuid-1', {status: 'SUCCESS', perfettoSql: fullSql});
    expect(store.getOrCreate('uuid-1').perfettoSql).toBe(fullSql);
  });

  test('shorter error does not overwrite existing full error', () => {
    const store = new QueryStore();
    const fullErr = 'no such table: events; line 12 col 4';
    store.getOrCreate('uuid-1', {status: 'FAILED', error: fullErr});
    store.getOrCreate('uuid-1', {status: 'FAILED', error: 'no such tabl…'});
    expect(store.getOrCreate('uuid-1').error).toBe(fullErr);
  });
});
