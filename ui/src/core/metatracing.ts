// Copyright (C) 2022  The Android Open Source Project
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

import {featureFlags} from './feature_flags';
import protos from '../protos';
import protobuf from 'protobufjs/minimal';

const METATRACING_BUFFER_SIZE = 100000;

export enum MetatraceTrackId {
  // 1 is reserved for the Trace Processor track.
  // Events emitted by the JS main thread.
  kMainThread = 2,
  // Async track for the status (e.g. "loading tracks") shown to the user
  // in the omnibox.
  kOmniboxStatus = 3,
}

const AOMT_FLAG = featureFlags.register({
  id: 'alwaysOnMetatracing',
  name: 'Enable always-on-metatracing',
  description: 'Enables trace events in the UI and trace processor',
  defaultValue: false,
});

const MT_DETAILED_FLAG = featureFlags.register({
  id: 'detailedMetatracing',
  name: 'Detailed metatracing',
  description: 'Enables recording additional events for trace event',
  defaultValue: false,
});

function getDefaultCategories(): protos.MetatraceCategories {
  if (MT_DETAILED_FLAG.get()) return protos.MetatraceCategories.ALL;
  return (
    protos.MetatraceCategories.QUERY_TIMELINE |
    protos.MetatraceCategories.API_TIMELINE
  );
}

function getInitialCategories(): protos.MetatraceCategories | undefined {
  if (!AOMT_FLAG.get()) return undefined;
  return getDefaultCategories();
}

let enabledCategories: protos.MetatraceCategories | undefined =
  getInitialCategories();

export function enableMetatracing(categories?: protos.MetatraceCategories) {
  enabledCategories =
    categories === undefined || categories === protos.MetatraceCategories.NONE
      ? getDefaultCategories()
      : categories;
}

export function disableMetatracingAndGetTrace(): Uint8Array {
  enabledCategories = undefined;
  return readMetatrace();
}

export function isMetatracingEnabled(): boolean {
  return enabledCategories !== undefined;
}

export function getEnabledMetatracingCategories():
  | protos.MetatraceCategories
  | undefined {
  return enabledCategories;
}

interface TraceEvent {
  eventName: string;
  startNs: number;
  durNs: number;
  track: MetatraceTrackId;
  args?: {[key: string]: string};
}

const traceEvents: TraceEvent[] = [];

function readMetatrace(): Uint8Array {
  const eventToPacket = (e: TraceEvent): Uint8Array => {
    const metatraceEvent = protos.PerfettoMetatrace.create({
      eventName: e.eventName,
      threadId: e.track,
      eventDurationNs: e.durNs,
    });
    for (const [key, value] of Object.entries(e.args ?? {})) {
      metatraceEvent.args.push(
        protos.PerfettoMetatrace.Arg.create({
          key,
          value,
        }),
      );
    }
    const PROTO_VARINT_TYPE = 0;
    const PROTO_LEN_DELIMITED_WIRE_TYPE = 2;
    const TRACE_PACKET_PROTO_TAG = (1 << 3) | PROTO_LEN_DELIMITED_WIRE_TYPE;
    const TRACE_PACKET_TIMESTAMP_TAG = (8 << 3) | PROTO_VARINT_TYPE;
    const TRACE_PACKET_CLOCK_ID_TAG = (58 << 3) | PROTO_VARINT_TYPE;
    const TRACE_PACKET_METATRACE_TAG =
      (49 << 3) | PROTO_LEN_DELIMITED_WIRE_TYPE;

    const wri = protobuf.Writer.create();
    wri.uint32(TRACE_PACKET_PROTO_TAG);
    wri.fork(); // Start of Trace Packet.
    wri.uint32(TRACE_PACKET_TIMESTAMP_TAG).int64(e.startNs);
    wri.uint32(TRACE_PACKET_CLOCK_ID_TAG).int32(1);
    wri
      .uint32(TRACE_PACKET_METATRACE_TAG)
      .bytes(protos.PerfettoMetatrace.encode(metatraceEvent).finish());
    wri.ldelim();
    return wri.finish();
  };
  const packets: Uint8Array[] = [];
  for (const event of traceEvents) {
    packets.push(eventToPacket(event));
  }
  const totalLength = packets.reduce((acc, arr) => acc + arr.length, 0);
  const trace = new Uint8Array(totalLength);
  let offset = 0;
  for (const packet of packets) {
    trace.set(packet, offset);
    offset += packet.length;
  }
  return trace;
}

interface TraceEventParams {
  track?: MetatraceTrackId;
  args?: {[key: string]: string};
}

export type TraceEventScope = {
  startNs: number;
  eventName: string;
  params?: TraceEventParams;
};

const correctedTimeOrigin = new Date().getTime() - performance.now();

function msToNs(ms: number) {
  return Math.round(ms * 1e6);
}

function now(): number {
  return msToNs(correctedTimeOrigin + performance.now());
}

export function traceEvent<T>(
  name: string,
  event: () => T,
  params?: TraceEventParams,
): T {
  const scope = traceEventBegin(name, params);
  try {
    const result = event();
    return result;
  } finally {
    traceEventEnd(scope);
  }
}

export function traceEventBegin(
  eventName: string,
  params?: TraceEventParams,
): TraceEventScope {
  return {
    eventName,
    startNs: now(),
    params: params,
  };
}

export function traceEventEnd(traceEvent: TraceEventScope) {
  if (!isMetatracingEnabled()) return;

  traceEvents.push({
    eventName: traceEvent.eventName,
    startNs: traceEvent.startNs,
    durNs: now() - traceEvent.startNs,
    track: traceEvent.params?.track ?? MetatraceTrackId.kMainThread,
    args: traceEvent.params?.args,
  });
  while (traceEvents.length > METATRACING_BUFFER_SIZE) {
    traceEvents.shift();
  }
}

// Flatten arbitrary values so they can be used as args in traceEvent() et al.
export function flattenArgs(
  input: unknown,
  parentKey = '',
): {[key: string]: string} {
  if (typeof input !== 'object' || input === null) {
    return {[parentKey]: String(input)};
  }

  if (Array.isArray(input)) {
    const result: Record<string, string> = {};

    (input as Array<unknown>).forEach((item, index) => {
      const arrayKey = `${parentKey}[${index}]`;
      Object.assign(result, flattenArgs(item, arrayKey));
    });

    return result;
  }

  const result: Record<string, string> = {};

  Object.entries(input as Record<string, unknown>).forEach(([key, value]) => {
    const newKey = parentKey ? `${parentKey}.${key}` : key;
    Object.assign(result, flattenArgs(value, newKey));
  });

  return result;
}
