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

import {
  RECORD_PLUGIN_SCHEMA,
  RECORD_SESSION_SCHEMA,
  SAVED_SESSION_SCHEMA,
  TARGET_SCHEMA,
} from './serialization_schema';

describe('RECORD_SESSION_SCHEMA', () => {
  it('uses defaults when empty object is provided', () => {
    const result = RECORD_SESSION_SCHEMA.parse({});

    expect(result.kind).toBe('probes');
    if (result.kind !== 'probes') throw new Error('unexpected kind');
    expect(result.mode).toBe('STOP_WHEN_FULL');
    expect(result.bufSizeKb).toBe(64 * 1024);
    expect(result.durationMs).toBe(10_000);
    expect(result.maxFileSizeMb).toBe(500);
    expect(result.fileWritePeriodMs).toBe(2500);
    expect(result.compression).toBe(false);
    expect(result.probes).toEqual({});
  });

  it('parses custom session config', () => {
    const result = RECORD_SESSION_SCHEMA.parse({
      kind: 'custom',
      customTraceConfigBase64: 'dGVzdA==',
      customConfigFileName: 'test.textproto',
    });

    expect(result.kind).toBe('custom');
    if (result.kind !== 'custom') throw new Error('Expected custom');
    expect(result.customTraceConfigBase64).toBe('dGVzdA==');
    expect(result.customConfigFileName).toBe('test.textproto');
  });
});

describe('SAVED_SESSION_SCHEMA', () => {
  it('uses defaults when config is missing', () => {
    const input = {
      name: 'My Saved Config',
    };

    const result = SAVED_SESSION_SCHEMA.parse(input);

    expect(result.name).toBe('My Saved Config');
    expect(result.config.kind).toBe('probes');
    if (result.config.kind !== 'probes') throw new Error('Expected probes');
    expect(result.config.mode).toBe('STOP_WHEN_FULL');
    expect(result.config.bufSizeKb).toBe(64 * 1024);
    expect(result.config.durationMs).toBe(10_000);
    expect(result.config.maxFileSizeMb).toBe(500);
    expect(result.config.fileWritePeriodMs).toBe(2500);
    expect(result.config.compression).toBe(false);
    expect(result.config.probes).toEqual({});
  });
});

describe('TARGET_SCHEMA', () => {
  it('uses defaults when empty object is provided', () => {
    const result = TARGET_SCHEMA.parse({});

    expect(result.platformId).toBeUndefined();
    expect(result.transportId).toBeUndefined();
    expect(result.targetId).toBeUndefined();
  });
});

describe('RECORD_PLUGIN_SCHEMA', () => {
  it('uses TARGET_SCHEMA defaults when target is omitted', () => {
    const result = RECORD_PLUGIN_SCHEMA.parse({});

    expect(result.target.platformId).toBeUndefined();
    expect(result.target.transportId).toBeUndefined();
    expect(result.target.targetId).toBeUndefined();
  });

  it('uses RECORD_SESSION_SCHEMA defaults when lastSession is omitted', () => {
    const result = RECORD_PLUGIN_SCHEMA.parse({});

    expect(result.lastSession.kind).toBe('probes');
    if (result.lastSession.kind !== 'probes') {
      throw new Error('Expected probes');
    }
    expect(result.lastSession.mode).toBe('STOP_WHEN_FULL');
    expect(result.lastSession.bufSizeKb).toBe(64 * 1024);
    expect(result.lastSession.durationMs).toBe(10_000);
    expect(result.lastSession.maxFileSizeMb).toBe(500);
    expect(result.lastSession.fileWritePeriodMs).toBe(2500);
    expect(result.lastSession.compression).toBe(false);
    expect(result.lastSession.probes).toEqual({});
  });
});
