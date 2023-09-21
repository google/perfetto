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

import protos from '../gen/protos';

// Aliases protos to avoid the super nested namespaces.
// See https://www.typescriptlang.org/docs/handbook/namespaces.html#aliases
import AndroidLogConfig = protos.perfetto.protos.AndroidLogConfig;
import AndroidLogId = protos.perfetto.protos.AndroidLogId;
import AndroidPowerConfig = protos.perfetto.protos.AndroidPowerConfig;
import BatteryCounters =
    protos.perfetto.protos.AndroidPowerConfig.BatteryCounters;
import BufferConfig = protos.perfetto.protos.TraceConfig.BufferConfig;
import ChromeConfig = protos.perfetto.protos.ChromeConfig;
import ComputeMetricArgs = protos.perfetto.protos.ComputeMetricArgs;
import ComputeMetricResult = protos.perfetto.protos.ComputeMetricResult;
import ConsumerPort = protos.perfetto.protos.ConsumerPort;
import DataSourceConfig = protos.perfetto.protos.DataSourceConfig;
import DataSourceDescriptor = protos.perfetto.protos.DataSourceDescriptor;
import DisableAndReadMetatraceResult =
    protos.perfetto.protos.DisableAndReadMetatraceResult;
import DisableTracingRequest = protos.perfetto.protos.DisableTracingRequest;
import DisableTracingResponse = protos.perfetto.protos.DisableTracingResponse;
import EnableMetatraceArgs = protos.perfetto.protos.EnableMetatraceArgs;
import EnableTracingRequest = protos.perfetto.protos.EnableTracingRequest;
import EnableTracingResponse = protos.perfetto.protos.EnableTracingResponse;
import FreeBuffersRequest = protos.perfetto.protos.FreeBuffersRequest;
import FreeBuffersResponse = protos.perfetto.protos.FreeBuffersResponse;
import FtraceConfig = protos.perfetto.protos.FtraceConfig;
import GetTraceStatsRequest = protos.perfetto.protos.GetTraceStatsRequest;
import GetTraceStatsResponse = protos.perfetto.protos.GetTraceStatsResponse;
import HeapprofdConfig = protos.perfetto.protos.HeapprofdConfig;
import IAndroidPowerConfig = protos.perfetto.protos.IAndroidPowerConfig;
import IBufferConfig = protos.perfetto.protos.TraceConfig.IBufferConfig;
import IBufferStats = protos.perfetto.protos.TraceStats.IBufferStats;
import IDisableTracingResponse = protos.perfetto.protos.IDisableTracingResponse;
import IEnableTracingResponse = protos.perfetto.protos.IEnableTracingResponse;
import IFreeBuffersResponse = protos.perfetto.protos.IFreeBuffersResponse;
import IGetTraceStatsResponse = protos.perfetto.protos.IGetTraceStatsResponse;
import IMethodInfo =
    protos.perfetto.protos.IPCFrame.BindServiceReply.IMethodInfo;
import IPCFrame = protos.perfetto.protos.IPCFrame;
import IProcessStatsConfig = protos.perfetto.protos.IProcessStatsConfig;
import IReadBuffersResponse = protos.perfetto.protos.IReadBuffersResponse;
import ISlice = protos.perfetto.protos.ReadBuffersResponse.ISlice;
import ISysStatsConfig = protos.perfetto.protos.ISysStatsConfig;
import ITraceConfig = protos.perfetto.protos.ITraceConfig;
import ITraceStats = protos.perfetto.protos.ITraceStats;
import JavaContinuousDumpConfig =
    protos.perfetto.protos.JavaHprofConfig.ContinuousDumpConfig;
import JavaHprofConfig = protos.perfetto.protos.JavaHprofConfig;
import MeminfoCounters = protos.perfetto.protos.MeminfoCounters;
import MetatraceCategories = protos.perfetto.protos.MetatraceCategories;
import NativeContinuousDumpConfig =
    protos.perfetto.protos.HeapprofdConfig.ContinuousDumpConfig;
import NetworkPacketTraceConfig =
    protos.perfetto.protos.NetworkPacketTraceConfig;
import PerfEventConfig = protos.perfetto.protos.PerfEventConfig;
import PerfEvents = protos.perfetto.protos.PerfEvents;
import PerfettoMetatrace = protos.perfetto.protos.PerfettoMetatrace;
import ProcessStatsConfig = protos.perfetto.protos.ProcessStatsConfig;
import QueryArgs = protos.perfetto.protos.QueryArgs;
import QueryResult = protos.perfetto.protos.QueryResult;
import QueryServiceStateRequest =
    protos.perfetto.protos.QueryServiceStateRequest;
import QueryServiceStateResponse =
    protos.perfetto.protos.QueryServiceStateResponse;
import ReadBuffersRequest = protos.perfetto.protos.ReadBuffersRequest;
import ReadBuffersResponse = protos.perfetto.protos.ReadBuffersResponse;
import ResetTraceProcessorArgs = protos.perfetto.protos.ResetTraceProcessorArgs;
import StatCounters = protos.perfetto.protos.SysStatsConfig.StatCounters;
import StatusResult = protos.perfetto.protos.StatusResult;
import SysStatsConfig = protos.perfetto.protos.SysStatsConfig;
import Trace = protos.perfetto.protos.Trace;
import TraceConfig = protos.perfetto.protos.TraceConfig;
import TracePacket = protos.perfetto.protos.TracePacket;
import TraceProcessorApiVersion =
    protos.perfetto.protos.TraceProcessorApiVersion;
import TraceProcessorRpc = protos.perfetto.protos.TraceProcessorRpc;
import TraceProcessorRpcStream = protos.perfetto.protos.TraceProcessorRpcStream;
import TrackEventConfig = protos.perfetto.protos.TrackEventConfig;
import VmstatCounters = protos.perfetto.protos.VmstatCounters;

export {
  AndroidLogConfig,
  AndroidLogId,
  AndroidPowerConfig,
  BatteryCounters,
  BufferConfig,
  ChromeConfig,
  ComputeMetricArgs,
  ComputeMetricResult,
  ConsumerPort,
  DataSourceConfig,
  DataSourceDescriptor,
  DisableAndReadMetatraceResult,
  DisableTracingRequest,
  DisableTracingResponse,
  EnableMetatraceArgs,
  EnableTracingRequest,
  EnableTracingResponse,
  FreeBuffersRequest,
  FreeBuffersResponse,
  FtraceConfig,
  GetTraceStatsRequest,
  GetTraceStatsResponse,
  HeapprofdConfig,
  IAndroidPowerConfig,
  IBufferConfig,
  IBufferStats,
  IDisableTracingResponse,
  IEnableTracingResponse,
  IFreeBuffersResponse,
  IGetTraceStatsResponse,
  IMethodInfo,
  IPCFrame,
  IProcessStatsConfig,
  IReadBuffersResponse,
  ISlice,
  ISysStatsConfig,
  ITraceConfig,
  ITraceStats,
  JavaContinuousDumpConfig,
  JavaHprofConfig,
  MeminfoCounters,
  MetatraceCategories,
  NativeContinuousDumpConfig,
  NetworkPacketTraceConfig,
  PerfettoMetatrace,
  PerfEventConfig,
  PerfEvents,
  ProcessStatsConfig,
  QueryArgs,
  QueryResult,
  QueryServiceStateRequest,
  QueryServiceStateResponse,
  ReadBuffersRequest,
  ReadBuffersResponse,
  ResetTraceProcessorArgs,
  StatCounters,
  StatusResult,
  SysStatsConfig,
  Trace,
  TraceConfig,
  TracePacket,
  TraceProcessorApiVersion,
  TraceProcessorRpc,
  TraceProcessorRpcStream,
  TrackEventConfig,
  VmstatCounters,

};
