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

import {defer, Deferred} from '../base/deferred';
import {assertExists, assertTrue} from '../base/logging';
import {
  ComputeMetricArgs,
  ComputeMetricResult,
  DisableAndReadMetatraceResult,
  EnableMetatraceArgs,
  MetatraceCategories,
  QueryArgs,
  QueryResult as ProtoQueryResult,
  RegisterSqlPackageArgs,
  ResetTraceProcessorArgs,
  TraceProcessorRpc,
  TraceProcessorRpcStream,
} from '../protos';
import {ProtoRingBuffer} from './proto_ring_buffer';
import {
  createQueryResult,
  QueryError,
  QueryResult,
  WritableQueryResult,
} from './query_result';
import TPM = TraceProcessorRpc.TraceProcessorMethod;
import {exists, Optional, Result} from '../base/utils';

export type EngineMode = 'WASM' | 'HTTP_RPC';
export type NewEngineMode = 'USE_HTTP_RPC_IF_AVAILABLE' | 'FORCE_BUILTIN_WASM';

// This is used to skip the decoding of queryResult from protobufjs and deal
// with it ourselves. See the comment below around `QueryResult.decode = ...`.
interface QueryResultBypass {
  rawQueryResult: Uint8Array;
}

export interface TraceProcessorConfig {
  cropTrackEvents: boolean;
  ingestFtraceInRawTable: boolean;
  analyzeTraceProtoContent: boolean;
  ftraceDropUntilAllCpusValid: boolean;
}

export interface Engine {
  readonly mode: EngineMode;
  readonly engineId: string;

  /**
   * Execute a query against the database, returning a promise that resolves
   * when the query has completed but rejected when the query fails for whatever
   * reason. On success, the promise will only resolve once all the resulting
   * rows have been received.
   *
   * The promise will be rejected if the query fails.
   *
   * @param sql The query to execute.
   * @param tag An optional tag used to trace the origin of the query.
   */
  query(sql: string, tag?: string): Promise<QueryResult>;

  /**
   * Execute a query against the database, returning a promise that resolves
   * when the query has completed or failed. The promise will never get
   * rejected, it will always successfully resolve. Use the returned wrapper
   * object to determine whether the query completed successfully.
   *
   * The promise will only resolve once all the resulting rows have been
   * received.
   *
   * @param sql The query to execute.
   * @param tag An optional tag used to trace the origin of the query.
   */
  tryQuery(sql: string, tag?: string): Promise<Result<QueryResult, Error>>;

  /**
   * Execute one or more metric and get the result.
   *
   * @param metrics The metrics to run.
   * @param format The format of the response.
   */
  computeMetric(
    metrics: string[],
    format: 'json' | 'prototext' | 'proto',
  ): Promise<string | Uint8Array>;

  enableMetatrace(categories?: MetatraceCategories): void;
  stopAndGetMetatrace(): Promise<DisableAndReadMetatraceResult>;

  getProxy(tag: string): EngineProxy;
  readonly numRequestsPending: number;
  readonly failed: Optional<string>;
}

// Abstract interface of a trace proccessor.
// This is the TypeScript equivalent of src/trace_processor/rpc.h.
// There are two concrete implementations:
//   1. WasmEngineProxy: creates a Wasm module and interacts over postMessage().
//   2. HttpRpcEngine: connects to an external `trace_processor_shell --httpd`.
//      and interacts via fetch().
// In both cases, we have a byte-oriented pipe to interact with TraceProcessor.
// The derived class is only expected to deal with these two functions:
// 1. Implement the abstract rpcSendRequestBytes() function, sending the
//    proto-encoded TraceProcessorRpc requests to the TraceProcessor instance.
// 2. Call onRpcResponseBytes() when response data is received.
export abstract class EngineBase implements Engine, Disposable {
  abstract readonly id: string;
  abstract readonly mode: EngineMode;
  private txSeqId = 0;
  private rxSeqId = 0;
  private rxBuf = new ProtoRingBuffer();
  private pendingParses = new Array<Deferred<void>>();
  private pendingEOFs = new Array<Deferred<void>>();
  private pendingResetTraceProcessors = new Array<Deferred<void>>();
  private pendingQueries = new Array<WritableQueryResult>();
  private pendingRestoreTables = new Array<Deferred<void>>();
  private pendingComputeMetrics = new Array<Deferred<string | Uint8Array>>();
  private pendingReadMetatrace?: Deferred<DisableAndReadMetatraceResult>;
  private pendingRegisterSqlPackage?: Deferred<void>;
  private _isMetatracingEnabled = false;
  private _numRequestsPending = 0;
  private _failed: Optional<string> = undefined;

  // TraceController sets this to raf.scheduleFullRedraw().
  onResponseReceived?: () => void;

  // Called to send data to the TraceProcessor instance. This turns into a
  // postMessage() or a HTTP request, depending on the Engine implementation.
  abstract rpcSendRequestBytes(data: Uint8Array): void;

  // Called when an inbound message is received by the Engine implementation
  // (e.g. onmessage for the Wasm case, on when HTTP replies are received for
  // the HTTP+RPC case).
  onRpcResponseBytes(dataWillBeRetained: Uint8Array) {
    // Note: when hitting the fastpath inside ProtoRingBuffer, the |data| buffer
    // is returned back by readMessage() (% subarray()-ing it) and held onto by
    // other classes (e.g., QueryResult). For both fetch() and Wasm we are fine
    // because every response creates a new buffer.
    this.rxBuf.append(dataWillBeRetained);
    for (;;) {
      const msg = this.rxBuf.readMessage();
      if (msg === undefined) break;
      this.onRpcResponseMessage(msg);
    }
  }

  // Parses a response message.
  // |rpcMsgEncoded| is a sub-array to to the start of a TraceProcessorRpc
  // proto-encoded message (without the proto preamble and varint size).
  private onRpcResponseMessage(rpcMsgEncoded: Uint8Array) {
    // Here we override the protobufjs-generated code to skip the parsing of the
    // new streaming QueryResult and instead passing it through like a buffer.
    // This is the overall problem: All trace processor responses are wrapped
    // into a perfetto.protos.TraceProcessorRpc proto message. In all cases %
    // TPM_QUERY_STREAMING, we want protobufjs to decode the proto bytes and
    // give us a structured object. In the case of TPM_QUERY_STREAMING, instead,
    // we want to deal with the proto parsing ourselves using the new
    // QueryResult.appendResultBatch() method, because that handled streaming
    // results more efficiently and skips several copies.
    // By overriding the decode method below, we achieve two things:
    // 1. We avoid protobufjs decoding the TraceProcessorRpc.query_result field.
    // 2. We stash (a view of) the original buffer into the |rawQueryResult| so
    //    the `case TPM_QUERY_STREAMING` below can take it.
    ProtoQueryResult.decode = (reader: protobuf.Reader, length: number) => {
      const res = ProtoQueryResult.create() as {} as QueryResultBypass;
      res.rawQueryResult = reader.buf.subarray(reader.pos, reader.pos + length);
      // All this works only if protobufjs returns the original ArrayBuffer
      // from |rpcMsgEncoded|. It should be always the case given the
      // current implementation. This check mainly guards against future
      // behavioral changes of protobufjs. We don't want to accidentally
      // hold onto some internal protobufjs buffer. We are fine holding
      // onto |rpcMsgEncoded| because those come from ProtoRingBuffer which
      // is buffer-retention-friendly.
      assertTrue(res.rawQueryResult.buffer === rpcMsgEncoded.buffer);
      reader.pos += length;
      return res as {} as ProtoQueryResult;
    };

    const rpc = TraceProcessorRpc.decode(rpcMsgEncoded);

    if (rpc.fatalError !== undefined && rpc.fatalError.length > 0) {
      this.fail(`${rpc.fatalError}`);
    }

    // Allow restarting sequences from zero (when reloading the browser).
    if (rpc.seq !== this.rxSeqId + 1 && this.rxSeqId !== 0 && rpc.seq !== 0) {
      // "(ERR:rpc_seq)" is intercepted by error_dialog.ts to show a more
      // graceful and actionable error.
      this.fail(
        `RPC sequence id mismatch ` +
          `cur=${rpc.seq} last=${this.rxSeqId} (ERR:rpc_seq)`,
      );
    }

    this.rxSeqId = rpc.seq;

    let isFinalResponse = true;

    switch (rpc.response) {
      case TPM.TPM_APPEND_TRACE_DATA:
        const appendResult = assertExists(rpc.appendResult);
        const pendingPromise = assertExists(this.pendingParses.shift());
        if (exists(appendResult.error) && appendResult.error.length > 0) {
          pendingPromise.reject(appendResult.error);
        } else {
          pendingPromise.resolve();
        }
        break;
      case TPM.TPM_FINALIZE_TRACE_DATA:
        assertExists(this.pendingEOFs.shift()).resolve();
        break;
      case TPM.TPM_RESET_TRACE_PROCESSOR:
        assertExists(this.pendingResetTraceProcessors.shift()).resolve();
        break;
      case TPM.TPM_RESTORE_INITIAL_TABLES:
        assertExists(this.pendingRestoreTables.shift()).resolve();
        break;
      case TPM.TPM_QUERY_STREAMING:
        const qRes = assertExists(rpc.queryResult) as {} as QueryResultBypass;
        const pendingQuery = assertExists(this.pendingQueries[0]);
        pendingQuery.appendResultBatch(qRes.rawQueryResult);
        if (pendingQuery.isComplete()) {
          this.pendingQueries.shift();
        } else {
          isFinalResponse = false;
        }
        break;
      case TPM.TPM_COMPUTE_METRIC:
        const metricRes = assertExists(rpc.metricResult) as ComputeMetricResult;
        const pendingComputeMetric = assertExists(
          this.pendingComputeMetrics.shift(),
        );
        if (exists(metricRes.error) && metricRes.error.length > 0) {
          const error = new QueryError(
            `ComputeMetric() error: ${metricRes.error}`,
            {
              query: 'COMPUTE_METRIC',
            },
          );
          pendingComputeMetric.reject(error);
        } else {
          const result =
            metricRes.metricsAsPrototext ??
            metricRes.metricsAsJson ??
            metricRes.metrics ??
            '';
          pendingComputeMetric.resolve(result);
        }
        break;
      case TPM.TPM_DISABLE_AND_READ_METATRACE:
        const metatraceRes = assertExists(
          rpc.metatrace,
        ) as DisableAndReadMetatraceResult;
        assertExists(this.pendingReadMetatrace).resolve(metatraceRes);
        this.pendingReadMetatrace = undefined;
        break;
      case TPM.TPM_REGISTER_SQL_PACKAGE:
        const registerResult = assertExists(rpc.registerSqlPackageResult);
        const res = assertExists(this.pendingRegisterSqlPackage);
        if (exists(registerResult.error) && registerResult.error.length > 0) {
          res.reject(registerResult.error);
        } else {
          res.resolve();
        }
        break;
      default:
        console.log(
          'Unexpected TraceProcessor response received: ',
          rpc.response,
        );
        break;
    } // switch(rpc.response);

    if (isFinalResponse) {
      --this._numRequestsPending;
    }

    this.onResponseReceived?.();
  }

  // TraceProcessor methods below this point.
  // The methods below are called by the various controllers in the UI and
  // deal with marshalling / unmarshaling requests to/from TraceProcessor.

  // Push trace data into the engine. The engine is supposed to automatically
  // figure out the type of the trace (JSON vs Protobuf).
  parse(data: Uint8Array): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingParses.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_APPEND_TRACE_DATA;
    rpc.appendTraceData = data;
    this.rpcSendRequest(rpc);
    return asyncRes; // Linearize with the worker.
  }

  // Notify the engine that we reached the end of the trace.
  // Called after the last parse() call.
  notifyEof(): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingEOFs.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_FINALIZE_TRACE_DATA;
    this.rpcSendRequest(rpc);
    return asyncRes; // Linearize with the worker.
  }

  // Updates the TraceProcessor Config. This method creates a new
  // TraceProcessor instance, so it should be called before passing any trace
  // data.
  resetTraceProcessor({
    cropTrackEvents,
    ingestFtraceInRawTable,
    analyzeTraceProtoContent,
    ftraceDropUntilAllCpusValid,
  }: TraceProcessorConfig): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingResetTraceProcessors.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_RESET_TRACE_PROCESSOR;
    const args = (rpc.resetTraceProcessorArgs = new ResetTraceProcessorArgs());
    args.dropTrackEventDataBefore = cropTrackEvents
      ? ResetTraceProcessorArgs.DropTrackEventDataBefore
          .TRACK_EVENT_RANGE_OF_INTEREST
      : ResetTraceProcessorArgs.DropTrackEventDataBefore.NO_DROP;
    args.ingestFtraceInRawTable = ingestFtraceInRawTable;
    args.analyzeTraceProtoContent = analyzeTraceProtoContent;
    args.ftraceDropUntilAllCpusValid = ftraceDropUntilAllCpusValid;
    this.rpcSendRequest(rpc);
    return asyncRes;
  }

  // Resets the trace processor state by destroying any table/views created by
  // the UI after loading.
  restoreInitialTables(): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingRestoreTables.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_RESTORE_INITIAL_TABLES;
    this.rpcSendRequest(rpc);
    return asyncRes; // Linearize with the worker.
  }

  // Shorthand for sending a compute metrics request to the engine.
  async computeMetric(
    metrics: string[],
    format: 'json' | 'prototext' | 'proto',
  ): Promise<string | Uint8Array> {
    const asyncRes = defer<string | Uint8Array>();
    this.pendingComputeMetrics.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_COMPUTE_METRIC;
    const args = (rpc.computeMetricArgs = new ComputeMetricArgs());
    args.metricNames = metrics;
    if (format === 'json') {
      args.format = ComputeMetricArgs.ResultFormat.JSON;
    } else if (format === 'prototext') {
      args.format = ComputeMetricArgs.ResultFormat.TEXTPROTO;
    } else if (format === 'proto') {
      args.format = ComputeMetricArgs.ResultFormat.BINARY_PROTOBUF;
    } else {
      throw new Error(`Unknown compute metric format ${format}`);
    }
    this.rpcSendRequest(rpc);
    return asyncRes;
  }

  // Issues a streaming query and retrieve results in batches.
  // The returned QueryResult object will be populated over time with batches
  // of rows (each batch conveys ~128KB of data and a variable number of rows).
  // The caller can decide whether to wait that all batches have been received
  // (by awaiting the returned object or calling result.waitAllRows()) or handle
  // the rows incrementally.
  //
  // Example usage:
  // const res = engine.execute('SELECT foo, bar FROM table');
  // console.log(res.numRows());  // Will print 0 because we didn't await.
  // await(res.waitAllRows());
  // console.log(res.numRows());  // Will print the total number of rows.
  //
  // for (const it = res.iter({foo: NUM, bar:STR}); it.valid(); it.next()) {
  //   console.log(it.foo, it.bar);
  // }
  //
  // Optional |tag| (usually a component name) can be provided to allow
  // attributing trace processor workload to different UI components.
  private streamingQuery(
    sqlQuery: string,
    tag?: string,
  ): Promise<QueryResult> & QueryResult {
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_QUERY_STREAMING;
    rpc.queryArgs = new QueryArgs();
    rpc.queryArgs.sqlQuery = sqlQuery;
    if (tag) {
      rpc.queryArgs.tag = tag;
    }
    const result = createQueryResult({
      query: sqlQuery,
    });
    this.pendingQueries.push(result);
    this.rpcSendRequest(rpc);
    return result;
  }

  // Wraps .streamingQuery(), captures errors and re-throws with current stack.
  //
  // Note: This function is less flexible than .execute() as it only returns a
  // promise which must be unwrapped before the QueryResult may be accessed.
  async query(sqlQuery: string, tag?: string): Promise<QueryResult> {
    try {
      return await this.streamingQuery(sqlQuery, tag);
    } catch (e) {
      // Replace the error's stack trace with the one from here
      // Note: It seems only V8 can trace the stack up the promise chain, so its
      // likely this stack won't be useful on !V8.
      // See
      // https://docs.google.com/document/d/13Sy_kBIJGP0XT34V1CV3nkWya4TwYx9L3Yv45LdGB6Q
      captureStackTrace(e);
      throw e;
    }
  }

  async tryQuery(
    sql: string,
    tag?: string,
  ): Promise<Result<QueryResult, Error>> {
    try {
      const result = await this.query(sql, tag);
      return {success: true, result};
    } catch (error: unknown) {
      // We know we only throw Error type objects so we can type assert safely
      return {success: false, error: error as Error};
    }
  }

  isMetatracingEnabled(): boolean {
    return this._isMetatracingEnabled;
  }

  enableMetatrace(categories?: MetatraceCategories) {
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_ENABLE_METATRACE;
    if (categories !== undefined && categories !== MetatraceCategories.NONE) {
      rpc.enableMetatraceArgs = new EnableMetatraceArgs();
      rpc.enableMetatraceArgs.categories = categories;
    }
    this._isMetatracingEnabled = true;
    this.rpcSendRequest(rpc);
  }

  stopAndGetMetatrace(): Promise<DisableAndReadMetatraceResult> {
    // If we are already finalising a metatrace, ignore the request.
    if (this.pendingReadMetatrace) {
      return Promise.reject(new Error('Already finalising a metatrace'));
    }

    const result = defer<DisableAndReadMetatraceResult>();

    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_DISABLE_AND_READ_METATRACE;
    this._isMetatracingEnabled = false;
    this.pendingReadMetatrace = result;
    this.rpcSendRequest(rpc);
    return result;
  }

  registerSqlPackages(p: {
    name: string;
    modules: {name: string; sql: string}[];
  }): Promise<void> {
    if (this.pendingRegisterSqlPackage) {
      return Promise.reject(new Error('Already finalising a metatrace'));
    }

    const result = defer<void>();

    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_REGISTER_SQL_PACKAGE;
    const args = (rpc.registerSqlPackageArgs = new RegisterSqlPackageArgs());
    args.packageName = p.name;
    args.modules = p.modules;
    args.allowOverride = true;
    this.pendingRegisterSqlPackage = result;
    this.rpcSendRequest(rpc);
    return result;
  }

  // Marshals the TraceProcessorRpc request arguments and sends the request
  // to the concrete Engine (Wasm or HTTP).
  private rpcSendRequest(rpc: TraceProcessorRpc) {
    rpc.seq = this.txSeqId++;
    // Each message is wrapped in a TraceProcessorRpcStream to add the varint
    // preamble with the size, which allows tokenization on the other end.
    const outerProto = TraceProcessorRpcStream.create();
    outerProto.msg.push(rpc);
    const buf = TraceProcessorRpcStream.encode(outerProto).finish();
    ++this._numRequestsPending;
    this.rpcSendRequestBytes(buf);
  }

  get engineId(): string {
    return this.id;
  }

  get numRequestsPending(): number {
    return this._numRequestsPending;
  }

  getProxy(tag: string): EngineProxy {
    return new EngineProxy(this, tag);
  }

  protected fail(reason: string) {
    this._failed = reason;
    throw new Error(reason);
  }

  get failed(): Optional<string> {
    return this._failed;
  }

  abstract [Symbol.dispose](): void;
}

// Lightweight engine proxy which annotates all queries with a tag
export class EngineProxy implements Engine, Disposable {
  private engine: EngineBase;
  private tag: string;
  private _isAlive: boolean;

  constructor(engine: EngineBase, tag: string) {
    this.engine = engine;
    this.tag = tag;
    this._isAlive = true;
  }

  async query(query: string, tag?: string): Promise<QueryResult> {
    if (!this._isAlive) {
      throw new Error(`EngineProxy ${this.tag} was disposed.`);
    }
    return await this.engine.query(query, tag);
  }

  async tryQuery(
    query: string,
    tag?: string,
  ): Promise<Result<QueryResult, Error>> {
    if (!this._isAlive) {
      return {
        success: false,
        error: new Error(`EngineProxy ${this.tag} was disposed.`),
      };
    }
    return await this.engine.tryQuery(query, tag);
  }

  async computeMetric(
    metrics: string[],
    format: 'json' | 'prototext' | 'proto',
  ): Promise<string | Uint8Array> {
    if (!this._isAlive) {
      return Promise.reject(new Error(`EngineProxy ${this.tag} was disposed.`));
    }
    return this.engine.computeMetric(metrics, format);
  }

  enableMetatrace(categories?: MetatraceCategories): void {
    this.engine.enableMetatrace(categories);
  }

  stopAndGetMetatrace(): Promise<DisableAndReadMetatraceResult> {
    return this.engine.stopAndGetMetatrace();
  }

  get engineId(): string {
    return this.engine.id;
  }

  getProxy(tag: string): EngineProxy {
    return this.engine.getProxy(`${this.tag}/${tag}`);
  }

  get numRequestsPending() {
    return this.engine.numRequestsPending;
  }

  get mode() {
    return this.engine.mode;
  }

  get failed() {
    return this.engine.failed;
  }

  [Symbol.dispose]() {
    this._isAlive = false;
  }
}

// Capture stack trace and attach to the given error object
function captureStackTrace(e: Error): void {
  const stack = new Error().stack;
  if ('captureStackTrace' in Error) {
    // V8 specific
    Error.captureStackTrace(e, captureStackTrace);
  } else {
    // Generic
    Object.defineProperty(e, 'stack', {
      value: stack,
      writable: true,
      configurable: true,
    });
  }
}

// A convenience interface to inject the App in Mithril components.
export interface EngineAttrs {
  engine: Engine;
}
