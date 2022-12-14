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

import * as protos from '../gen/protos';

// Aliases protos to avoid the super nested namespaces.
// See https://www.typescriptlang.org/docs/handbook/namespaces.html#aliases
import AndroidLogConfig = protos.perfetto.protos.AndroidLogConfig;
import AndroidPowerConfig = protos.perfetto.protos.AndroidPowerConfig;
import AndroidLogId = protos.perfetto.protos.AndroidLogId;
import BatteryCounters =
    protos.perfetto.protos.AndroidPowerConfig.BatteryCounters;
import BufferConfig = protos.perfetto.protos.TraceConfig.BufferConfig;
import ChromeConfig = protos.perfetto.protos.ChromeConfig;
import TrackEventConfig = protos.perfetto.protos.TrackEventConfig;
import ConsumerPort = protos.perfetto.protos.ConsumerPort;
import NativeContinuousDumpConfig =
    protos.perfetto.protos.HeapprofdConfig.ContinuousDumpConfig;
import JavaContinuousDumpConfig =
    protos.perfetto.protos.JavaHprofConfig.ContinuousDumpConfig;
import DataSourceConfig = protos.perfetto.protos.DataSourceConfig;
import DataSourceDescriptor = protos.perfetto.protos.DataSourceDescriptor;
import FtraceConfig = protos.perfetto.protos.FtraceConfig;
import HeapprofdConfig = protos.perfetto.protos.HeapprofdConfig;
import JavaHprofConfig = protos.perfetto.protos.JavaHprofConfig;
import IAndroidPowerConfig = protos.perfetto.protos.IAndroidPowerConfig;
import IBufferConfig = protos.perfetto.protos.TraceConfig.IBufferConfig;
import IProcessStatsConfig = protos.perfetto.protos.IProcessStatsConfig;
import ISysStatsConfig = protos.perfetto.protos.ISysStatsConfig;
import ITraceConfig = protos.perfetto.protos.ITraceConfig;
import MeminfoCounters = protos.perfetto.protos.MeminfoCounters;
import ProcessStatsConfig = protos.perfetto.protos.ProcessStatsConfig;
import StatCounters = protos.perfetto.protos.SysStatsConfig.StatCounters;
import SysStatsConfig = protos.perfetto.protos.SysStatsConfig;
import TraceConfig = protos.perfetto.protos.TraceConfig;
import VmstatCounters = protos.perfetto.protos.VmstatCounters;
import IPCFrame = protos.perfetto.protos.IPCFrame;
import IMethodInfo =
    protos.perfetto.protos.IPCFrame.BindServiceReply.IMethodInfo;
import IBufferStats = protos.perfetto.protos.TraceStats.IBufferStats;
import ISlice = protos.perfetto.protos.ReadBuffersResponse.ISlice;
import EnableTracingRequest = protos.perfetto.protos.EnableTracingRequest;
import DisableTracingRequest = protos.perfetto.protos.DisableTracingRequest;
import GetTraceStatsRequest = protos.perfetto.protos.GetTraceStatsRequest;
import FreeBuffersRequest = protos.perfetto.protos.FreeBuffersRequest;
import ReadBuffersRequest = protos.perfetto.protos.ReadBuffersRequest;
import QueryServiceStateRequest =
    protos.perfetto.protos.QueryServiceStateRequest;
import EnableTracingResponse = protos.perfetto.protos.EnableTracingResponse;
import DisableTracingResponse = protos.perfetto.protos.DisableTracingResponse;
import GetTraceStatsResponse = protos.perfetto.protos.GetTraceStatsResponse;
import FreeBuffersResponse = protos.perfetto.protos.FreeBuffersResponse;
import ReadBuffersResponse = protos.perfetto.protos.ReadBuffersResponse;
import QueryServiceStateResponse =
    protos.perfetto.protos.QueryServiceStateResponse;
// Trace Processor protos.
import QueryArgs = protos.perfetto.protos.QueryArgs;
import ResetTraceProcessorArgs = protos.perfetto.protos.ResetTraceProcessorArgs;
import StatusResult = protos.perfetto.protos.StatusResult;
import ComputeMetricArgs = protos.perfetto.protos.ComputeMetricArgs;
import ComputeMetricResult = protos.perfetto.protos.ComputeMetricResult;
import DisableAndReadMetatraceResult =
    protos.perfetto.protos.DisableAndReadMetatraceResult;
import Trace = protos.perfetto.protos.Trace;
import TracePacket = protos.perfetto.protos.TracePacket;
import PerfettoMetatrace = protos.perfetto.protos.PerfettoMetatrace;

export {
  AndroidLogConfig,
  AndroidLogId,
  AndroidPowerConfig,
  BatteryCounters,
  BufferConfig,
  ChromeConfig,
  ConsumerPort,
  ComputeMetricArgs,
  ComputeMetricResult,
  DataSourceConfig,
  DisableAndReadMetatraceResult,
  DataSourceDescriptor,
  DisableTracingRequest,
  DisableTracingResponse,
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
  IMethodInfo,
  IPCFrame,
  IProcessStatsConfig,
  ISlice,
  ISysStatsConfig,
  ITraceConfig,
  JavaContinuousDumpConfig,
  JavaHprofConfig,
  MeminfoCounters,
  NativeContinuousDumpConfig,
  ProcessStatsConfig,
  PerfettoMetatrace,
  ReadBuffersRequest,
  ReadBuffersResponse,
  QueryServiceStateRequest,
  QueryServiceStateResponse,
  QueryArgs,
  ResetTraceProcessorArgs,
  StatCounters,
  StatusResult,
  SysStatsConfig,
  Trace,
  TraceConfig,
  TrackEventConfig,
  TracePacket,
  VmstatCounters,
};
