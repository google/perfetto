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

import protos from '../protos';
import {defer, Deferred} from '../base/deferred';
import {assertExists, assertTrue, assertUnreachable} from '../base/logging';
import {ProtoRingBuffer} from './proto_ring_buffer';
import {
  createQueryResult,
  QueryError,
  QueryResult,
  WritableQueryResult,
} from './query_result';
import TPM = protos.TraceProcessorRpc.TraceProcessorMethod;
import {exists} from '../base/utils';
import {errResult, okResult, Result} from '../base/result';

export type EngineMode = 'WASM' | 'HTTP_RPC';
export type NewEngineMode = 'USE_HTTP_RPC_IF_AVAILABLE' | 'FORCE_BUILTIN_WASM';

// This is used to skip the decoding of queryResult from protobufjs and deal
// with it ourselves. See the comment below around `QueryResult.decode = ...`.
interface QueryResultBypass {
  rawQueryResult: Uint8Array;
}

export interface TraceProcessorConfig {
  // When true, the trace processor will only tokenize the trace without
  // performing a full parse. This is a performance optimization that allows for
  // a faster, albeit partial, import of the trace.
  tokenizeOnly: boolean;
  cropTrackEvents: boolean;
  ingestFtraceInRawTable: boolean;
  analyzeTraceProtoContent: boolean;
  ftraceDropUntilAllCpusValid: boolean;
  extraParsingDescriptors?: ReadonlyArray<Uint8Array>;
  forceFullSort: boolean;
}

const QUERY_LOG_BUFFER_SIZE = 100;

interface QueryLog {
  readonly tag?: string;
  readonly query: string;
  readonly startTime: number;
  readonly endTime?: number;
  readonly success?: boolean;
}

export interface Engine {
  readonly mode: EngineMode;
  readonly engineId: string;

  /**
   * A list of the most recent queries along with their start times, end times
   * and success status (if completed).
   */
  readonly queryLog: ReadonlyArray<QueryLog>;

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
  query(sql: string): Promise<QueryResult>;

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
  tryQuery(sql: string): Promise<Result<QueryResult>>;

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

  summarizeTrace(
    summarySpecs: protos.TraceSummarySpec[] | string[],
    metricIds: string[] | undefined,
    metadataId: string | undefined,
    format: 'prototext' | 'proto',
  ): Promise<protos.TraceSummaryResult>;

  enableMetatrace(categories?: protos.MetatraceCategories): void;
  stopAndGetMetatrace(): Promise<protos.DisableAndReadMetatraceResult>;

  analyzeStructuredQuery(
    structuredQueries: protos.PerfettoSqlStructuredQuery[],
  ): Promise<protos.AnalyzeStructuredQueryResult>;

  getProxy(tag: string): EngineProxy;
  readonly numRequestsPending: number;
  readonly failed: string | undefined;
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
  private pendingReadMetatrace?: Deferred<protos.DisableAndReadMetatraceResult>;
  private pendingRegisterSqlPackage?: Deferred<void>;
  private pendingAnalyzeStructuredQueries?: Deferred<protos.AnalyzeStructuredQueryResult>;
  private pendingTraceSummary?: Deferred<protos.TraceSummaryResult>;
  private _numRequestsPending = 0;
  private _failed: string | undefined = undefined;
  private _queryLog: Array<QueryLog> = [];

  get queryLog(): ReadonlyArray<QueryLog> {
    return this._queryLog;
  }

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
    // into a TraceProcessorRpc proto message. In all cases %
    // TPM_QUERY_STREAMING, we want protobufjs to decode the proto bytes and
    // give us a structured object. In the case of TPM_QUERY_STREAMING, instead,
    // we want to deal with the proto parsing ourselves using the new
    // QueryResult.appendResultBatch() method, because that handled streaming
    // results more efficiently and skips several copies.
    // By overriding the decode method below, we achieve two things:
    // 1. We avoid protobufjs decoding the TraceProcessorRpc.query_result field.
    // 2. We stash (a view of) the original buffer into the |rawQueryResult| so
    //    the `case TPM_QUERY_STREAMING` below can take it.
    protos.QueryResult.decode = (reader: protobuf.Reader, length: number) => {
      const res = protos.QueryResult.create() as {} as QueryResultBypass;
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
      return res as {} as protos.QueryResult;
    };

    const rpc = protos.TraceProcessorRpc.decode(rpcMsgEncoded);

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
      case TPM.TPM_APPEND_TRACE_DATA: {
        const appendResult = assertExists(rpc.appendResult);
        const pendingPromise = assertExists(this.pendingParses.shift());
        if (exists(appendResult.error) && appendResult.error.length > 0) {
          pendingPromise.reject(appendResult.error);
        } else {
          pendingPromise.resolve();
        }
        break;
      }
      case TPM.TPM_FINALIZE_TRACE_DATA: {
        const finalizeResult = assertExists(rpc.finalizeDataResult);
        const pendingPromise = assertExists(this.pendingEOFs.shift());
        if (exists(finalizeResult.error) && finalizeResult.error.length > 0) {
          pendingPromise.reject(finalizeResult.error);
        } else {
          pendingPromise.resolve();
        }
        break;
      }
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
        const metricRes = assertExists(
          rpc.metricResult,
        ) as protos.ComputeMetricResult;
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
        ) as protos.DisableAndReadMetatraceResult;
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
      case TPM.TPM_SUMMARIZE_TRACE:
        const summaryRes = assertExists(
          rpc.traceSummaryResult,
        ) as protos.TraceSummaryResult;
        assertExists(this.pendingTraceSummary).resolve(summaryRes);
        this.pendingTraceSummary = undefined;
        break;
      case TPM.TPM_ANALYZE_STRUCTURED_QUERY:
        const analyzeRes = assertExists(
          rpc.analyzeStructuredQueryResult,
        ) as {} as protos.AnalyzeStructuredQueryResult;
        const x = assertExists(this.pendingAnalyzeStructuredQueries);
        x.resolve(analyzeRes);
        this.pendingAnalyzeStructuredQueries = undefined;
        break;
      case TPM.TPM_ENABLE_METATRACE:
        // We don't have any pending promises for this request so just
        // return.
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
    const rpc = protos.TraceProcessorRpc.create();
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
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_FINALIZE_TRACE_DATA;
    this.rpcSendRequest(rpc);
    return asyncRes; // Linearize with the worker.
  }

  // Updates the TraceProcessor Config. This method creates a new
  // TraceProcessor instance, so it should be called before passing any trace
  // data.
  resetTraceProcessor({
    tokenizeOnly,
    cropTrackEvents,
    ingestFtraceInRawTable,
    analyzeTraceProtoContent,
    ftraceDropUntilAllCpusValid,
    extraParsingDescriptors,
    forceFullSort,
  }: TraceProcessorConfig): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingResetTraceProcessors.push(asyncRes);
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_RESET_TRACE_PROCESSOR;
    const args = (rpc.resetTraceProcessorArgs =
      new protos.ResetTraceProcessorArgs());
    args.dropTrackEventDataBefore = cropTrackEvents
      ? protos.ResetTraceProcessorArgs.DropTrackEventDataBefore
          .TRACK_EVENT_RANGE_OF_INTEREST
      : protos.ResetTraceProcessorArgs.DropTrackEventDataBefore.NO_DROP;
    args.ingestFtraceInRawTable = ingestFtraceInRawTable;
    args.analyzeTraceProtoContent = analyzeTraceProtoContent;
    args.ftraceDropUntilAllCpusValid = ftraceDropUntilAllCpusValid;
    args.sortingMode = forceFullSort
      ? protos.ResetTraceProcessorArgs.SortingMode.FORCE_FULL_SORT
      : protos.ResetTraceProcessorArgs.SortingMode.DEFAULT_HEURISTICS;
    args.parsingMode = tokenizeOnly
      ? protos.ResetTraceProcessorArgs.ParsingMode.TOKENIZE_ONLY
      : protos.ResetTraceProcessorArgs.ParsingMode.DEFAULT;
    // If extraParsingDescriptors is defined, create a mutable copy for the
    // protobuf object; otherwise, pass an empty array.
    args.extraParsingDescriptors = extraParsingDescriptors
      ? [...extraParsingDescriptors]
      : [];
    this.rpcSendRequest(rpc);
    return asyncRes;
  }

  // Resets the trace processor state by destroying any table/views created by
  // the UI after loading.
  restoreInitialTables(): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingRestoreTables.push(asyncRes);
    const rpc = protos.TraceProcessorRpc.create();
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
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_COMPUTE_METRIC;
    const args = (rpc.computeMetricArgs = new protos.ComputeMetricArgs());
    args.metricNames = metrics;
    if (format === 'json') {
      args.format = protos.ComputeMetricArgs.ResultFormat.JSON;
    } else if (format === 'prototext') {
      args.format = protos.ComputeMetricArgs.ResultFormat.TEXTPROTO;
    } else if (format === 'proto') {
      args.format = protos.ComputeMetricArgs.ResultFormat.BINARY_PROTOBUF;
    } else {
      throw new Error(`Unknown compute metric format ${format}`);
    }
    this.rpcSendRequest(rpc);
    return asyncRes;
  }

  summarizeTrace(
    summarySpecs: protos.TraceSummarySpec[] | string[],
    metricIds: string[] | undefined,
    metadataId: string | undefined,
    format: 'prototext' | 'proto',
  ): Promise<protos.TraceSummaryResult> {
    if (this.pendingTraceSummary) {
      return Promise.reject(new Error('Already summarizing trace'));
    }
    if (summarySpecs.length === 0) {
      return Promise.reject(new Error('No summary specs provided'));
    }
    const result = defer<protos.TraceSummaryResult>();
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_SUMMARIZE_TRACE;
    const args = (rpc.traceSummaryArgs = new protos.TraceSummaryArgs());
    const computationSpec = new protos.TraceSummaryArgs.ComputationSpec();
    if (metricIds) {
      computationSpec.metricIds = metricIds;
    } else {
      computationSpec.runAllMetrics = true;
    }
    if (metadataId) {
      computationSpec.metadataQueryId = metadataId;
    }
    args.computationSpec = computationSpec;

    if (typeof summarySpecs[0] === 'string') {
      args.textprotoSpecs = summarySpecs as string[];
    } else {
      args.protoSpecs = summarySpecs as protos.TraceSummarySpec[];
    }

    switch (format) {
      case 'prototext':
        args.outputFormat = protos.TraceSummaryArgs.Format.TEXTPROTO;
        break;
      case 'proto':
        args.outputFormat = protos.TraceSummaryArgs.Format.BINARY_PROTOBUF;
        break;
      default:
        assertUnreachable(format);
    }
    this.pendingTraceSummary = result;
    this.rpcSendRequest(rpc);
    return result;
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
  // NOTE: the only reason why this is public is so that Winscope (which uses a
  // fork of our codebase) can invoke this directly. See commit msg of #3051.
  streamingQuery(result: WritableQueryResult, sqlQuery: string, tag?: string) {
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_QUERY_STREAMING;
    rpc.queryArgs = new protos.QueryArgs();
    rpc.queryArgs.sqlQuery = sqlQuery;
    rpc.queryArgs.tag = tag;
    this.pendingQueries.push(result);
    this.rpcSendRequest(rpc);
  }

  private logQueryStart(
    query: string,
    tag?: string,
  ): {
    endTime?: number;
    success?: boolean;
  } {
    const startTime = performance.now();
    const queryLog: QueryLog = {query, tag, startTime};
    this._queryLog.push(queryLog);
    if (this._queryLog.length > QUERY_LOG_BUFFER_SIZE) {
      this._queryLog.shift();
    }
    return queryLog;
  }

  // Wraps .streamingQuery(), captures errors and re-throws with current stack.
  //
  // Note: This function is less flexible than .execute() as it only returns a
  // promise which must be unwrapped before the QueryResult may be accessed.
  async query(sqlQuery: string, tag?: string): Promise<QueryResult> {
    const queryLog = this.logQueryStart(sqlQuery, tag);
    try {
      const result = createQueryResult({query: sqlQuery, tag});
      this.streamingQuery(result, sqlQuery, tag);
      const resolvedResult = await result;
      queryLog.success = true;
      return resolvedResult;
    } catch (e) {
      // Replace the error's stack trace with the one from here
      // Note: It seems only V8 can trace the stack up the promise chain, so its
      // likely this stack won't be useful on !V8.
      // See
      // https://docs.google.com/document/d/13Sy_kBIJGP0XT34V1CV3nkWya4TwYx9L3Yv45LdGB6Q
      captureStackTrace(e);
      queryLog.success = false;
      throw e;
    } finally {
      queryLog.endTime = performance.now();
    }
  }

  async tryQuery(sql: string, tag?: string): Promise<Result<QueryResult>> {
    try {
      const result = await this.query(sql, tag);
      return okResult(result);
    } catch (error) {
      const msg = 'message' in error ? `${error.message}` : `${error}`;
      return errResult(msg);
    }
  }

  enableMetatrace(categories?: protos.MetatraceCategories) {
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_ENABLE_METATRACE;
    if (
      categories !== undefined &&
      categories !== protos.MetatraceCategories.NONE
    ) {
      rpc.enableMetatraceArgs = new protos.EnableMetatraceArgs();
      rpc.enableMetatraceArgs.categories = categories;
    }
    this.rpcSendRequest(rpc);
  }

  stopAndGetMetatrace(): Promise<protos.DisableAndReadMetatraceResult> {
    // If we are already finalising a metatrace, ignore the request.
    if (this.pendingReadMetatrace) {
      return Promise.reject(new Error('Already finalising a metatrace'));
    }

    const result = defer<protos.DisableAndReadMetatraceResult>();

    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_DISABLE_AND_READ_METATRACE;
    this.pendingReadMetatrace = result;
    this.rpcSendRequest(rpc);
    return result;
  }

  registerSqlPackages(pkg: {
    name: string;
    modules: {name: string; sql: string}[];
  }): Promise<void> {
    if (this.pendingRegisterSqlPackage) {
      return Promise.reject(new Error('Already registering SQL package'));
    }

    const result = defer<void>();

    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_REGISTER_SQL_PACKAGE;
    const args = (rpc.registerSqlPackageArgs =
      new protos.RegisterSqlPackageArgs());
    args.packageName = pkg.name;
    args.modules = pkg.modules;
    args.allowOverride = true;
    this.pendingRegisterSqlPackage = result;
    this.rpcSendRequest(rpc);
    return result;
  }

  analyzeStructuredQuery(
    structuredQueries: protos.PerfettoSqlStructuredQuery[],
  ): Promise<protos.AnalyzeStructuredQueryResult> {
    if (this.pendingAnalyzeStructuredQueries) {
      return Promise.reject(new Error('Already analyzing structured queries'));
    }
    const result = defer<protos.AnalyzeStructuredQueryResult>();
    const rpc = protos.TraceProcessorRpc.create();
    rpc.request = TPM.TPM_ANALYZE_STRUCTURED_QUERY;
    const args = (rpc.analyzeStructuredQueryArgs =
      new protos.AnalyzeStructuredQueryArgs());
    args.queries = structuredQueries;
    this.pendingAnalyzeStructuredQueries = result;
    this.rpcSendRequest(rpc);
    return result;
  }

  // Marshals the TraceProcessorRpc request arguments and sends the request
  // to the concrete Engine (Wasm or HTTP).
  private rpcSendRequest(rpc: protos.TraceProcessorRpc) {
    rpc.seq = this.txSeqId++;
    // Each message is wrapped in a TraceProcessorRpcStream to add the varint
    // preamble with the size, which allows tokenization on the other end.
    const outerProto = protos.TraceProcessorRpcStream.create();
    outerProto.msg.push(rpc);
    const buf = protos.TraceProcessorRpcStream.encode(outerProto).finish();
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

  get failed(): string | undefined {
    return this._failed;
  }

  abstract [Symbol.dispose](): void;
}

// Lightweight engine proxy which annotates all queries with a tag
export class EngineProxy implements Engine, Disposable {
  private engine: EngineBase;
  private disposed = false;
  private tag: string;

  get queryLog() {
    return this.engine.queryLog;
  }

  constructor(engine: EngineBase, tag: string) {
    this.engine = engine;
    this.tag = tag;
  }

  async query(query: string): Promise<QueryResult> {
    if (this.disposed) {
      // If we are disposed (the trace was closed), return an empty QueryResult
      // that will never see any data or EOF. We can't do otherwise or it will
      // cause crashes to code calling firstRow() and expecting data.
      return createQueryResult({query, tag: this.tag});
    }
    return await this.engine.query(query, this.tag);
  }

  async tryQuery(query: string): Promise<Result<QueryResult>> {
    if (this.disposed) {
      return errResult(`EngineProxy ${this.tag} was disposed`);
    }
    return await this.engine.tryQuery(query);
  }

  async computeMetric(
    metrics: string[],
    format: 'json' | 'prototext' | 'proto',
  ): Promise<string | Uint8Array> {
    if (this.disposed) {
      return defer<string>(); // Return a promise that will hang forever.
    }
    return this.engine.computeMetric(metrics, format);
  }

  summarizeTrace(
    summarySpecs: protos.TraceSummarySpec[] | string[],
    metricIds: string[] | undefined,
    metadataId: string | undefined,
    format: 'prototext' | 'proto',
  ): Promise<protos.TraceSummaryResult> {
    return this.engine.summarizeTrace(
      summarySpecs,
      metricIds,
      metadataId,
      format,
    );
  }

  enableMetatrace(categories?: protos.MetatraceCategories): void {
    this.engine.enableMetatrace(categories);
  }

  stopAndGetMetatrace(): Promise<protos.DisableAndReadMetatraceResult> {
    return this.engine.stopAndGetMetatrace();
  }

  analyzeStructuredQuery(
    structuredQueries: protos.PerfettoSqlStructuredQuery[],
  ): Promise<protos.AnalyzeStructuredQueryResult> {
    return this.engine.analyzeStructuredQuery(structuredQueries);
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
    this.disposed = true;
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
