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
import AndroidCujsPlugin from '../../com.android.AndroidCujs';
import type {PinRequest} from './pinRequest';
import {PinRequestType} from './pinRequest';
import {execCuj} from './pinCujMetricHandler';
import {execMissedFramesDuringCuj} from './pinCujScoped';
import {execFullTraceMissedFrames} from './fullTraceJankMetricHandler';
import {execBlockingCall} from './pinBlockingCall';
import {execNotificationsBlockingCall} from './pinNotificationsBlockingCall';
import {execProcessTracks} from './simpleProcessMetricHandler';
import {execGlobalDmaHeap} from './pinGlobalDmaHeapSizeMetricsHandler';

const JANK_CUJ_QUERY_PRECONDITIONS = `
  INCLUDE PERFETTO MODULE android.cujs.base;
  INCLUDE PERFETTO MODULE android.cujs.frames;
  INCLUDE PERFETTO MODULE android.critical_blocking_calls;
  INCLUDE PERFETTO MODULE android.frame_blocking_calls.blocking_calls_aggregation;
`;

type PinRequestOf<T extends PinRequestType> = Extract<PinRequest, {type: T}>;

/**
 * A type-safe dispatch table: one executor per PinRequestType, each receiving
 * its narrowed variant. Exhaustive by construction — adding a new
 * PinRequestType is a compile error here until its executor is added.
 */
type PinRequestExecutors = {
  [T in PinRequestType]: (ctx: Trace, req: PinRequestOf<T>) => Promise<void>;
};

const EXECUTORS: PinRequestExecutors = {
  [PinRequestType.AllJankCujs]: (ctx) =>
    ctx.plugins.getPlugin(AndroidCujsPlugin).pinJankCujs(ctx),
  [PinRequestType.AllLatencyCujs]: (ctx) =>
    ctx.plugins.getPlugin(AndroidCujsPlugin).pinLatencyCujs(ctx),
  [PinRequestType.Cuj]: execCuj,
  [PinRequestType.MissedFramesDuringCuj]: execMissedFramesDuringCuj,
  [PinRequestType.FullTraceMissedFrames]: execFullTraceMissedFrames,
  [PinRequestType.BlockingCall]: execBlockingCall,
  [PinRequestType.NotificationsBlockingCall]: execNotificationsBlockingCall,
  [PinRequestType.ProcessTracks]: execProcessTracks,
  [PinRequestType.GlobalDmaHeap]: execGlobalDmaHeap,
};

/**
 * Executes a single PinRequest by dispatching to its executor.
 *
 * @param {Trace} ctx Trace context.
 * @param {PinRequest} req The request to execute.
 */
export async function executePinRequest(
  ctx: Trace,
  req: PinRequest,
): Promise<void> {
  // Single contained cast: indexing by the union widens the param type.
  const exec = EXECUTORS[req.type] as (
    ctx: Trace,
    req: PinRequest,
  ) => Promise<void>;
  await exec(ctx, req);
}

/**
 * Executes a list of PinRequests in order. When the list is non-empty, the jank
 * CUJ query preconditions are run once up front (preserving the previous
 * `callHandlers` behavior).
 *
 * @param {Trace} ctx Trace context.
 * @param {PinRequest[]} reqs The requests to execute.
 */
export async function executePinRequests(
  ctx: Trace,
  reqs: PinRequest[],
): Promise<void> {
  if (reqs.length === 0) {
    return;
  }
  await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
  for (const req of reqs) {
    await executePinRequest(ctx, req);
  }
}
