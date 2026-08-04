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

import type {Trace} from '../../../public/trace';
import {
  PinIntentKind,
  ProcessMemoryType,
  type MetricHandler,
  type PinIntent,
} from './metricUtils';
import {pinCujInstance} from './pinCujMetricHandler';
import {pinCujScopedJankInstance} from './pinCujScoped';
import {pinBlockingCallHandlerInstance} from './pinBlockingCall';
import {pinNotificationsBlockingCallHandlerInstance} from './pinNotificationsBlockingCall';
import {pinFullTraceJankInstance} from './fullTraceJankMetricHandler';
import {pinGlobalDmaHeapSizeMetricsInstance} from './pinGlobalDmaHeapSizeMetricsHandler';
import {pinHeapSizeMetricsInstance} from './pinHeapSizeMetricsHandler';
import {pinBitmapMetricsInstance} from './pinBitmapMetricsHandler';
import {pinDirtyMemoryMetricsInstance} from './pinDirtyMemoryMetricsHandler';
import {pinGPUMemoryMetricsInstance} from './pinGPUMemoryMetricsHandler';
import {pinActivityOrBinderLeaksMetricsInstance} from './pinActivityOrBinderLeaksMetricsHandler';
import {pinHardwareBufferMemoryMetricsInstance} from './pinHardwareBufferMemoryMetricsHandler';

/*
 * PinRequest Execution Flow:
 *
 *  +-----------------------------------------------------------------+
 *  | Clients (Startup Commands, UI Deep Links, External Consoles)    |
 *  +-----------------------------------------------------------------+
 *                                   |
 *                                   | raw args (JSON, strings, dicts)
 *                                   v
 *  +-----------------------------------------------------------------+
 *  | normalizePinIntent(arg) [pinRequest.ts]                         |
 *  |   - Delegates regex matching & dictionary parsing to handlers   |
 *  |   - Deduplicates identical requests                             |
 *  +-----------------------------------------------------------------+
 *                                   |
 *                                   | PinIntent[] (Discriminated Union)
 *                                   v
 *  +-----------------------------------------------------------------+
 *  | executePinIntents(ctx, intents) [executor.ts]                   |
 *  |   - Dispatches each intent.kind to its MetricHandler            |
 *  |   - Executes query preconditions directly in handlers as needed |
 *  +-----------------------------------------------------------------+
 */

export const JANK_CUJ_QUERY_PRECONDITIONS = `
  INCLUDE PERFETTO MODULE android.cujs.frames;
  INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
  INCLUDE PERFETTO MODULE android.critical_blocking_calls;
`;

const PROCESS_MEMORY_HANDLERS: Record<ProcessMemoryType, MetricHandler> = {
  [ProcessMemoryType.HeapSize]: pinHeapSizeMetricsInstance,
  [ProcessMemoryType.BitmapMemory]: pinBitmapMetricsInstance,
  [ProcessMemoryType.DirtyMemory]: pinDirtyMemoryMetricsInstance,
  [ProcessMemoryType.GpuMemory]: pinGPUMemoryMetricsInstance,
  [ProcessMemoryType.ActivityOrBinderLeaks]:
    pinActivityOrBinderLeaksMetricsInstance,
  [ProcessMemoryType.HardwareBufferMemory]:
    pinHardwareBufferMemoryMetricsInstance,
};

/**
 * Creates and executes a PinIntent to pin all Android Jank CUJs.
 *
 * @param {Trace} ctx Trace context.
 */
export async function pinJankCujs(ctx: Trace): Promise<void> {
  await executePinIntent(ctx, {
    kind: PinIntentKind.Cuj,
    cujName: '*',
  });
}

/**
 * Creates and executes a PinIntent to pin all Android Latency CUJs.
 *
 * @param {Trace} ctx Trace context.
 */
export async function pinLatencyCujs(ctx: Trace): Promise<void> {
  await executePinIntent(ctx, {
    kind: PinIntentKind.Cuj,
    cujName: '*',
  });
}

/**
 * Executes a single PinIntent.
 */
export async function executePinIntent(
  ctx: Trace,
  intent: PinIntent,
): Promise<void> {
  switch (intent.kind) {
    case PinIntentKind.Cuj:
      await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
      await pinCujInstance.addMetricTrack(intent, ctx);
      break;
    case PinIntentKind.CujScopedJank:
      await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
      await pinCujScopedJankInstance.addMetricTrack(intent, ctx);
      break;
    case PinIntentKind.CujBlockingCall:
      await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
      await pinBlockingCallHandlerInstance.addMetricTrack(intent, ctx);
      break;
    case PinIntentKind.NotificationBlockingCall:
      await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
      await pinNotificationsBlockingCallHandlerInstance.addMetricTrack(
        intent,
        ctx,
      );
      break;
    case PinIntentKind.FullTraceJank:
      await pinFullTraceJankInstance.addMetricTrack(intent, ctx);
      break;
    case PinIntentKind.ProcessMemory: {
      const handler: MetricHandler | undefined =
        PROCESS_MEMORY_HANDLERS[intent.memoryType];
      if (handler !== undefined) {
        await handler.addMetricTrack({process: intent.process}, ctx);
      }
      break;
    }
    case PinIntentKind.GlobalDmaHeap:
      await pinGlobalDmaHeapSizeMetricsInstance.addMetricTrack({}, ctx);
      break;
  }
}

export const executePinRequest = executePinIntent;

/**
 * Executes a list of PinIntents in order.
 */
export async function executePinIntents(
  ctx: Trace,
  intents: PinIntent[],
): Promise<void> {
  for (const intent of intents) {
    await executePinIntent(ctx, intent);
  }
}

export const executePinRequests = executePinIntents;
