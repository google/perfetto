// Copyright (C) 2019 The Android Open Source Project
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

import {perfetto} from '../gen/protos';

export interface Typed {
  type: string;
}

export interface ReadBuffersResponse extends
    Typed, perfetto.protos.IReadBuffersResponse {}
export interface EnableTracingResponse extends
    Typed, perfetto.protos.IEnableTracingResponse {}
export interface GetTraceStatsResponse extends
    Typed, perfetto.protos.IGetTraceStatsResponse {}
export interface FreeBuffersResponse extends
    Typed, perfetto.protos.IFreeBuffersResponse {}
export interface GetCategoriesResponse extends Typed {}
export interface DisableTracingResponse extends
    Typed, perfetto.protos.IDisableTracingResponse {}

export type ConsumerPortResponse =
    EnableTracingResponse|ReadBuffersResponse|GetTraceStatsResponse|
    GetCategoriesResponse|FreeBuffersResponse|DisableTracingResponse;

export function isReadBuffersResponse(obj: Typed): obj is ReadBuffersResponse {
  return obj.type === 'ReadBuffersResponse';
}

export function isEnableTracingResponse(obj: Typed):
    obj is EnableTracingResponse {
  return obj.type === 'EnableTracingResponse';
}

export function isGetTraceStatsResponse(obj: Typed):
    obj is GetTraceStatsResponse {
  return obj.type === 'GetTraceStatsResponse';
}

export function isGetCategoriesResponse(obj: Typed):
    obj is GetCategoriesResponse {
  return obj.type === 'GetCategoriesResponse';
}

export function isFreeBuffersResponse(obj: Typed): obj is FreeBuffersResponse {
  return obj.type === 'FreeBuffersResponse';
}

export function isDisableTracingResponse(obj: Typed):
    obj is DisableTracingResponse {
  return obj.type === 'DisableTracingResponse';
}
