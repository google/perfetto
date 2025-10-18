// Copyright (C) 2025 The Android Open Source Project
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

import type {TraceInfo} from '../public/trace_info';
import {EngineBase, EngineProxy} from '../trace_processor/engine';
import {createProxy} from '../base/utils';
import type {AppImpl} from './app_impl';
import type {TraceStream} from './trace_stream';

// IMPORTANT: All jest.mock(...) calls must appear before importing the module
// under test so that the mocks may replace its dependencies.

// Mock metatracing utilities to be disabled by default (simplifies setup)
jest.mock('./metatracing', () => ({
  isMetatracingEnabled: () => false,
  getEnabledMetatracingCategories: () => undefined,
}));

// Mock cache manager to avoid hitting storage
jest.mock('./cache_manager', () => ({
  cacheTrace: jest.fn(async () => false),
}));

// Mock Router to avoid touching the real URL
const routerNavigateMock = jest.fn();
jest.mock('../core/router', () => ({
  Router: {
    navigate: routerNavigateMock,
    parseUrl: jest.fn(() => ({page: '/'})),
  },
}));

// Lightweight query result helpers used by the Engine mock
function mockQueryResult(rows: unknown[]) {
  return {
    numRows: () => rows.length,
    firstRow: () => rows[0],
    iter: () => {
      let i = 0;
      return createProxy(rows[i] ?? {}, {
        valid: () => i < rows.length,
        next: () => {
          i += 1;
        },
      });
    },
  };
}

// Shared fake engine base class used by both wasm & http-rpc engines
class FakeEngine {
  onResponseReceived: (() => void) | undefined;
  id: string;
  constructor(id: string) {
    this.id = id;
  }
  resetTraceProcessor = jest.fn();
  parse = jest.fn(async (_data: Uint8Array) => {});
  notifyEof = jest.fn(async () => {});
  registerSqlPackages = jest.fn(async () => {});
  enableMetatrace = jest.fn(async () => {});
  restoreInitialTables = jest.fn(async () => {});

  // Provide for the essential queries used by load_trace.ts
  async query(sql: string): Promise<unknown> {
    if (/from\s+trace_bounds/ims.test(sql)) {
      // Start/end are in nanoseconds (arbitrary but consistent numbers)
      return mockQueryResult([
        {startTs: 1_000_000_000n, endTs: 2_000_000_000n},
      ]);
    }
    if (/from\s+clock_snapshot/ims.test(sql)) {
      return mockQueryResult([]);
    }
    if (/name\s*=\s*'timezone_off_mins'/ims.test(sql)) {
      return mockQueryResult([{tzOffMin: null}]);
    }
    if (/from metadata\b.*'tracing_started_ns'/ims.test(sql)) {
      return mockQueryResult([
        {name: 'tracing_started_ns', intValue: 1_000_000_000n},
        {name: 'tracing_disabled_ns', intValue: 2_000_000_000n},
      ]);
    }
    if (/from\s+metadata\b.*\btrace_type\b/ims.test(sql)) {
      return mockQueryResult([]);
    }
    if (/from\s+metadata\b.*\btrace_uuid\b/ims.test(sql)) {
      return mockQueryResult([]);
    }
    if (/from\s+stats\b/ims.test(sql)) {
      return mockQueryResult([{errs: 0}]);
    }
    if (/select\s+\*\s+from\s+ftrace_event\b/ims.test(sql)) {
      return mockQueryResult([]);
    }
    if (/select\s+min\(ts\).*\bfrom\s+ftrace_event\b/ims.test(sql)) {
      return mockQueryResult([{start: null, end: null}]);
    }
    // Any other query (includes, function creation, etc.) => return empty
    return mockQueryResult([{}]);
  }

  /** Extract the fake engine from an engine-proxy. */
  static from(engineProxy: EngineProxy): FakeEngine {
    return engineProxy['engine'] as unknown as FakeEngine;
  }
}

// Mock the engine implementations
jest.mock('../trace_processor/wasm_engine_proxy', () => {
  return {
    WasmEngineProxy: class MockWasmEngineProxy extends FakeEngine {
      constructor(id: string) {
        super(id);
      }
    },
  };
});
let httpRpcConnected = false;
const httpRpcHostPort = '127.0.0.1:9001';
jest.mock('../trace_processor/http_rpc_engine', () => {
  class MockHttpRpcEngine extends FakeEngine {
    constructor(id: string) {
      super(id);
    }
    static async checkConnection() {
      return {connected: httpRpcConnected};
    }
    static get hostAndPort() {
      return httpRpcHostPort;
    }
  }
  return {HttpRpcEngine: MockHttpRpcEngine};
});

// Mock TraceStreams: tests can inject the next instance to be returned
let nextFileStream: TraceStream | undefined;
let nextBufferStream: TraceStream | undefined;
let nextHttpStream: TraceStream | undefined;
let nextMultipleFilesStream: TraceStream | undefined;

jest.mock('../core/trace_stream', () => ({
  TraceFileStream: jest.fn(() => nextFileStream),
  TraceBufferStream: jest.fn(() => nextBufferStream),
  TraceHttpStream: jest.fn(() => nextHttpStream),
  TraceMultipleFilesStream: jest.fn(() => nextMultipleFilesStream),
}));

// Mock TraceImpl so we don't pull in the whole UI
jest.mock('./trace_impl', () => ({
  TraceImpl: {
    createInstanceForCore: jest.fn(
      (_app: AppImpl, engine: EngineBase, traceInfo: TraceInfo) => {
        return {
          engine: new EngineProxy(engine, 'test'),
          traceInfo,
          timeline: {updateVisibleTime: jest.fn()},
          minimap: {load: jest.fn(async () => {})},
          notes: {addNote: jest.fn()},
          onTraceReady: {notify: jest.fn(async () => {})},
          tabs: {defaultTabs: [], showTab: jest.fn()},
          commands: {
            hasStartupCommands: jest.fn(() => false),
            runStartupCommands: jest.fn(async () => {}),
          },
          omnibox: {
            showStatusMessage: jest.fn(),
            disablePrompts: jest.fn(() => ({[Symbol.dispose]: () => {}})),
          },
        };
      },
    ),
  },
}));

// import the function under test after installing dependency mocks
import {loadTrace} from './load_trace';

function mockStream(chunks: Uint8Array[], bytesTotal?: number) {
  let i = 0;
  let bytesRead = 0;
  const total = bytesTotal ?? chunks.reduce((n, c) => n + c.byteLength, 0);
  return {
    readChunk: jest.fn(async () => {
      const eof = i >= chunks.length - 1;
      const data = chunks[i];
      bytesRead += data.byteLength;
      i++;
      return {
        data,
        eof,
        bytesRead,
        bytesTotal: total,
      };
    }),
  } as TraceStream;
}

function mockApp(): AppImpl {
  return {
    httpRpc: {
      newEngineMode: 'USE_HTTP_RPC_IF_AVAILABLE',
      httpRpcAvailable: false,
    },
    extraParsingDescriptors: [],
    extraSqlPackages: [],
    omnibox: {showStatusMessage: jest.fn()},
    setActiveTrace: jest.fn(),
    plugins: {onTraceLoad: jest.fn(async () => {})},
    raf: {scheduleFullRedraw: jest.fn()},
  } as unknown as AppImpl;
}

describe('loadTrace()', () => {
  let app: AppImpl;

  beforeEach(() => {
    app = mockApp();
    httpRpcConnected = false;
    nextFileStream = undefined;
    nextBufferStream = undefined;
    nextHttpStream = undefined;
    nextMultipleFilesStream = undefined;
  });

  afterEach(() => {
    jest.clearAllMocks();
  });

  afterAll(() => {
    jest.unmock('./metatracing');
    jest.unmock('./cache_manager');
    jest.unmock('./trace_impl');
    jest.unmock('../core/router');
    jest.unmock('../core/trace_stream');
    jest.unmock('../trace_processor/http_rpc_engine');
    jest.unmock('../trace_processor/wasm_engine_proxy');
  });

  test('should load from FILE source', async () => {
    const chunk1 = new Uint8Array([1, 2, 3]);
    const chunk2 = new Uint8Array([4, 5]);
    nextFileStream = mockStream([chunk1, chunk2], 8);

    const file = {name: 'mytrace.pftrace', size: 6_000_000} as unknown as File;
    const trace = await loadTrace(app, {type: 'FILE', file});

    const engine = FakeEngine.from(trace.engine);
    expect(engine.parse).toHaveBeenCalledTimes(2);
    expect(engine.parse).toHaveBeenNthCalledWith(1, chunk1);
    expect(engine.parse).toHaveBeenNthCalledWith(2, chunk2);
    expect(engine.notifyEof).toHaveBeenCalledTimes(1);

    // Ensure we created a trace and navigated to the viewer page
    expect(trace.traceInfo.traceTitle).toBe('mytrace.pftrace (6 MB)');

    expect(routerNavigateMock).toHaveBeenCalledWith(
      expect.stringContaining('/viewer?local_cache_key='),
    );
  });

  test('should load from ARRAY_BUFFER source', async () => {
    const chunk1 = new Uint8Array([10]);
    const chunk2 = new Uint8Array([11, 12]);
    nextBufferStream = mockStream([chunk1, chunk2], 3);

    const buffer = new ArrayBuffer(5_000_000);
    const trace = await loadTrace(app, {
      type: 'ARRAY_BUFFER',
      buffer,
      title: 'buffered',
      url: 'https://example.com/t',
    });

    const engine = FakeEngine.from(trace.engine)!;
    expect(engine.parse).toHaveBeenCalledTimes(2);
    expect(engine.notifyEof).toHaveBeenCalledTimes(1);

    expect(trace.traceInfo.traceTitle).toBe('buffered (5 MB)');
    expect(trace.traceInfo.traceUrl).toBe('https://example.com/t');
  });

  test('should load from URL source', async () => {
    const chunk1 = new Uint8Array([1]);
    const chunk2 = new Uint8Array([2]);
    nextHttpStream = mockStream([chunk1, chunk2], 2);

    const trace = await loadTrace(app, {
      type: 'URL',
      url: 'https://example.com/dir/trace.perfetto-trace',
    });

    const engine = FakeEngine.from(trace.engine)!;
    expect(engine.parse).toHaveBeenCalledTimes(2);
    expect(engine.notifyEof).toHaveBeenCalledTimes(1);

    expect(trace.traceInfo.traceTitle).toBe('trace.perfetto-trace');
    expect(trace.traceInfo.traceUrl).toBe(
      'https://example.com/dir/trace.perfetto-trace',
    );
  });

  test('should load from STREAM source', async () => {
    const chunk1 = new Uint8Array([7, 8]);
    const chunk2 = new Uint8Array([9]);
    const stream = mockStream([chunk1, chunk2], 3);

    const trace = await loadTrace(app, {type: 'STREAM', stream});

    const engine = FakeEngine.from(trace.engine)!;
    expect(engine.parse).toHaveBeenCalledTimes(2);
    expect(engine.notifyEof).toHaveBeenCalledTimes(1);
  });

  test('should load from HTTP_RPC source', async () => {
    // Make RPC available so createEngine picks HttpRpcEngine
    httpRpcConnected = true;

    const trace = await loadTrace(app, {type: 'HTTP_RPC'});

    const engine = FakeEngine.from(trace.engine)!;
    // There's nothing to parse in this scenario as the trace processor shell has done that
    expect(engine.parse).not.toHaveBeenCalled();
    expect(engine.notifyEof).not.toHaveBeenCalled();
    expect(engine.restoreInitialTables).toHaveBeenCalledTimes(1);

    expect(trace.traceInfo.traceTitle).toBe(`RPC @ ${httpRpcHostPort}`);
  });

  test('should load from MULTIPLE_FILES source', async () => {
    const chunk1 = new Uint8Array([0]);
    const chunk2 = new Uint8Array([1, 2, 3, 4]);
    nextMultipleFilesStream = mockStream([chunk1, chunk2], 5);

    const files = [
      {name: 'part1.pftrace', size: 1024},
      {name: 'part2.pftrace', size: 2048},
    ] as unknown as File[];

    const trace = await loadTrace(app, {type: 'MULTIPLE_FILES', files});

    const engine = FakeEngine.from(trace.engine)!;
    expect(engine.parse).toHaveBeenCalledTimes(2);
    expect(engine.notifyEof).toHaveBeenCalledTimes(1);
  });
});
