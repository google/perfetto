// Copyright 2014 The Chromium Authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ref https://github.com/ChromeDevTools/devtools-frontend/blob/main/front_end/models/trace/types/TraceEvents.ts
// docs https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview

type TBeginPhase = 'B';
type TEndPhase = 'E';
type TCompletePhase = 'X';
type TInstantPhase = 'I';
type TMarkPhase = 'R';
type TAsyncBeginPhase = 'b';
type TAsyncEndPhase = 'e';
type TAsyncInstancePhase = 'n';
type TFlowStartPhase = 's';
type TFlowEndPhase = 'f';
type TFlowStepPhase = 't';

type TProcessID = number | string;
type TThreadID = number | string;

type TBaseTraceEvent = {
  /**
   * The name of the event, as displayed in Chart.
   */
  name: string;
  /**
   *  The process ID for the process that output this event.
   */
  pid: TProcessID;
  /**
   *  The thread ID for the process that output this event.
   */
  tid?: TThreadID;
  /**
   *  The tracing clock timestamp of the event. The timestamps are provided at microsecond granularity.
   */
  ts: number;
  /**
   * Any arguments provided for the event. Some of the event types have required argument fields, otherwise, you can put any information you wish in here. The arguments are displayed in Trace Viewer when you view an event in the analysis section.
   */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  args?: Record<string, any>;
  /**
   * internal data, which provides extra information for us to transform data back
   */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  _internal?: Record<string, any> & {
    raw: TTraceEvent[];
  };
};

export type TBeginTraceEvent = TBaseTraceEvent & {
  ph: TBeginPhase;
};

export type TEndTraceEvent = TBaseTraceEvent & {
  ph: TEndPhase;
};

export type TDurationTraceEvent = TBeginTraceEvent | TEndTraceEvent;

export type TCompleteTraceEvent = TBaseTraceEvent & {
  ph: TCompletePhase;
  dur: number;
};

export type TInstanceTraceEvent = TBaseTraceEvent & {
  ph: TInstantPhase;
};

export type TMarkTraceEvent = TBaseTraceEvent & {
  ph: TMarkPhase;
};

export type TAsyncBeginTraceEvent = TBaseTraceEvent & {
  ph: TAsyncBeginPhase;
  id: string;
  cat: string;
};

export type TAsyncEndTraceEvent = TBaseTraceEvent & {
  ph: TAsyncEndPhase;
  id: string;
  cat: string;
};

export type TAsyncInstanceTraceEvent = TBaseTraceEvent & {
  ph: TAsyncInstancePhase;
  id: string;
  cat: string;
};

export type TFlowStartTraceEvent = TBaseTraceEvent & {
  ph: TFlowStartPhase;
  id: string;
};

export type TFlowEndTraceEvent = TBaseTraceEvent & {
  ph: TFlowEndPhase;
  id: string;
};

export type TFlowStepTraceEvent = TBaseTraceEvent & {
  ph: TFlowStepPhase;
  id: string;
};

export type TAsyncTraceEvent =
  | TAsyncBeginTraceEvent
  | TAsyncEndTraceEvent
  | TAsyncInstanceTraceEvent;

export type TFlowTraceEvent =
  | TFlowStartTraceEvent
  | TFlowEndTraceEvent
  | TFlowStepTraceEvent;

export type TTraceEvent =
  | TDurationTraceEvent
  | TCompleteTraceEvent
  | TInstanceTraceEvent
  | TMarkTraceEvent
  | TAsyncTraceEvent
  | TFlowTraceEvent;
