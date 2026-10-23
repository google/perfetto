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

import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {type time, Time} from '../base/time';
import {createFakeTraceImpl} from './fake_trace_impl';

// Live trace plugins grow traceInfo.end in place: the timeline must follow.
test('lets the timeline show a grown trace end', () => {
  const trace = createFakeTraceImpl(); // Spans [0s, 10s].
  (trace.traceInfo as {end: time}).end = Time.fromSeconds(20);

  trace.timeline.setVisibleWindow(
    HighPrecisionTimeSpan.fromTime(Time.fromSeconds(15), Time.fromSeconds(20)),
  );
  expect(trace.timeline.visibleWindow.end.integral).toBe(Time.fromSeconds(20));
});
