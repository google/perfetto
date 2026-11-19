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

import {createFakeTraceImpl} from './fake_trace_impl';

// Change detector for com.google.LiveStream, an out-of-tree plugin managed by
// xldrx@. It shows running jobs as a growing trace. The plugin grows the trace
// by writing traceInfo.end, which needs the timeline to read it on each use,
// not cache it. If this changes on purpose, delete this test and tell us, so
// the plugin can adapt.
test('the timeline reads the trace end on each use', () => {
  const trace = createFakeTraceImpl();
  const end = trace.traceInfo.end;
  const reads = vi.spyOn(trace.traceInfo, 'end', 'get').mockReturnValue(end);

  trace.timeline.pan(1);
  expect(reads).toHaveBeenCalled();
});
