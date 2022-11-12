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

import {PerfettoMetatrace, Trace, TracePacket} from '../common/protos';
import {perfetto} from '../gen/protos';

import {featureFlags} from './feature_flags';
import {toNs} from './time';

const METATRACING_BUFFER_SIZE = 100000;
const JS_THREAD_ID = 2;

import MetatraceCategories = perfetto.protos.MetatraceCategories;

const AOMT_FLAG = featureFlags.register({
  id: 'alwaysOnMetatracing',
  name: 'Enable always-on-metatracing',
  description: 'Enables trace events in the UI and trace processor',
  defaultValue: false,
});

const AOMT_DETAILED_FLAG = featureFlags.register({
  id: 'alwaysOnMetatracing_detailed',
  name: 'Detailed always-on-metatracing',
  description: 'Enables recording additional events for trace event',
  defaultValue: false,
});

function getInitialCategories(): MetatraceCategories|undefined {
  if (!AOMT_FLAG.get()) return undefined;
  if (AOMT_DETAILED_FLAG.get()) return MetatraceCategories.ALL;
  return MetatraceCategories.TOPLEVEL;
}

let enabledCategories: MetatraceCategories|undefined = getInitialCategories();

export function enableMetatracing(categories?: MetatraceCategories) {
  enabledCategories = categories || MetatraceCategories.ALL;
}

export function disableMetatracingAndGetTrace(): Uint8Array {
  enabledCategories = undefined;
  return readMetatrace();
}

export function isMetatracingEnabled(): boolean {
  return enabledCategories !== undefined;
}

export function getEnabledMetatracingCategories(): MetatraceCategories|
    undefined {
  return enabledCategories;
}

interface TraceEvent {
  eventName: string;
  startNs: number;
  durNs: number;
}

const traceEvents: TraceEvent[] = [];

function readMetatrace(): Uint8Array {
  const eventToPacket = (e: TraceEvent): TracePacket => {
    return TracePacket.create({
      timestamp: e.startNs,
      timestampClockId: 1,
      perfettoMetatrace: PerfettoMetatrace.create({
        eventName: e.eventName,
        threadId: JS_THREAD_ID,
        eventDurationNs: e.durNs,
      }),
    });
  };
  const packets: TracePacket[] = [];
  for (const event of traceEvents) {
    packets.push(eventToPacket(event));
  }
  const trace = Trace.create({
    packet: packets,
  });
  return Trace.encode(trace).finish();
}

export type TraceEventScope = {
  startNs: number, eventName: string;
};

const correctedTimeOrigin = new Date().getTime() - performance.now();

function now(): number {
  return toNs((correctedTimeOrigin + performance.now()) / 1000);
}

export function traceEventBegin(eventName: string): TraceEventScope {
  return {
    eventName,
    startNs: now(),
  };
}

export function traceEventEnd(traceEvent: TraceEventScope) {
  if (!isMetatracingEnabled()) return;

  traceEvents.push({
    eventName: traceEvent.eventName,
    startNs: traceEvent.startNs,
    durNs: now() - traceEvent.startNs,
  });
  while (traceEvents.length > METATRACING_BUFFER_SIZE) {
    traceEvents.shift();
  }
}
