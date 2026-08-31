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
import {PinIntentKind, type PinIntent} from './pinIntent';
import {
  addJankCUJDebugTrack,
  addLatencyCUJDebugTrack,
} from '../../com.android.AndroidCujs';
import {pinBlockingCallHandlerInstance} from './pinBlockingCall';
import {pinCujInstance} from './pinCujMetricHandler';
import {pinCujScopedJankInstance} from './pinCujScoped';
import {pinFullTraceJankInstance} from './fullTraceJankMetricHandler';

const JANK_CUJ_QUERY_PRECONDITIONS = `
  INCLUDE PERFETTO MODULE android.cujs.frames;
  INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;
  INCLUDE PERFETTO MODULE android.critical_blocking_calls;
`;

export async function executePinIntent(
  ctx: Trace,
  intent: PinIntent,
): Promise<void> {
  switch (intent.kind) {
    case PinIntentKind.Cuj:
      if (intent.cujName === '*') {
        await ctx.engine.query(JANK_CUJ_QUERY_PRECONDITIONS);
        await addJankCUJDebugTrack(ctx, 'Jank CUJs');
        await addLatencyCUJDebugTrack(ctx, 'Latency CUJs');
      } else {
        await pinCujInstance.addMetricTrack(intent, ctx);
      }
      break;
    case PinIntentKind.CujScopedJank:
      await pinCujScopedJankInstance.addMetricTrack(intent, ctx);
      break;
    case PinIntentKind.CujBlockingCall:
      await pinBlockingCallHandlerInstance.addMetricTrack(intent, ctx);
      break;
    case PinIntentKind.FullTraceJank:
      await pinFullTraceJankInstance.addMetricTrack(intent, ctx);
      break;
  }
}

export async function executePinIntents(
  ctx: Trace,
  intents: PinIntent[],
): Promise<void> {
  for (const intent of intents) {
    await executePinIntent(ctx, intent);
  }
}
