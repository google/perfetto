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
import {pinCujInstance} from './pinCujMetricHandler';
import {pinCujScopedJankInstance} from './pinCujScoped';
import {pinFullTraceJankInstance} from './fullTraceJankMetricHandler';
import {pinBlockingCallHandlerInstance} from './pinBlockingCall';
import {pinNotificationsBlockingCallHandlerInstance} from './pinNotificationsBlockingCall';
import {pinGlobalDmaHeapSizeMetricsInstance} from './pinGlobalDmaHeapSizeMetricsHandler';

/**
 * SQL run once before pinning any CUJ-derived track, so the tables the handlers
 * query are available. Exported so the legacy Crystalball path in index.ts can
 * share the same preconditions.
 */
export const JANK_CUJ_QUERY_PRECONDITIONS = `
  SELECT RUN_METRIC('android/android_blocking_calls_cuj_metric.sql');
`;

/**
 * Narrows the `PinRequest` union to the single variant whose discriminant is
 * `T`. E.g. `PinRequestOf<PinRequestType.Cuj>` is `CujPinRequest`. Used to give
 * each entry in the executor table a precisely-typed request parameter.
 */
type PinRequestOf<T extends PinRequestType> = Extract<PinRequest, {type: T}>;

/**
 * A type-safe dispatch table: one executor per PinRequestType, each receiving
 * its narrowed variant. Exhaustive by construction — adding a new
 * PinRequestType is a compile error here until its executor is added. Each
 * executor delegates to the existing `MetricHandler` instance (or the
 * AndroidCujs plugin), whose `addMetricTrack` input the request payload mirrors.
 */
type PinRequestExecutors = {
  [T in PinRequestType]: (
    ctx: Trace,
    req: PinRequestOf<T>,
  ) => void | Promise<void>;
};

const EXECUTORS: PinRequestExecutors = {
  [PinRequestType.AllJankCujs]: (ctx) =>
    ctx.plugins.getPlugin(AndroidCujsPlugin).pinJankCujs(ctx),
  [PinRequestType.AllLatencyCujs]: (ctx) =>
    ctx.plugins.getPlugin(AndroidCujsPlugin).pinLatencyCujs(ctx),
  [PinRequestType.Cuj]: (ctx, req) => pinCujInstance.addMetricTrack(req, ctx),
  [PinRequestType.MissedFramesDuringCuj]: (ctx, req) =>
    pinCujScopedJankInstance.addMetricTrack(req, ctx),
  [PinRequestType.FullTraceMissedFrames]: (ctx, req) =>
    pinFullTraceJankInstance.addMetricTrack(req, ctx),
  [PinRequestType.BlockingCall]: (ctx, req) =>
    pinBlockingCallHandlerInstance.addMetricTrack(req, ctx),
  [PinRequestType.NotificationsBlockingCall]: (ctx, req) =>
    pinNotificationsBlockingCallHandlerInstance.addMetricTrack(req, ctx),
  [PinRequestType.GlobalDmaHeap]: (ctx, req) =>
    pinGlobalDmaHeapSizeMetricsInstance.addMetricTrack(req, ctx),
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
  ) => void | Promise<void>;
  await exec(ctx, req);
}

/**
 * Executes a list of PinRequests in order. When the list is non-empty, the jank
 * CUJ query preconditions are run once up front.
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
