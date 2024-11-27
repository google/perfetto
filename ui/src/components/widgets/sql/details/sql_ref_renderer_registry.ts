// Copyright (C) 2024 The Android Open Source Project
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

import {Engine} from '../../../../trace_processor/engine';
import type {RenderedValue, SqlIdRefRenderer} from './details';

// Type-safe helper to create a SqlIdRefRenderer, which ensures that the
// type returned from the fetch is the same type that renderer takes.
export function createSqlIdRefRenderer<Data extends {}>(
  fetch: (engine: Engine, id: bigint) => Promise<Data>,
  render: (data: Data) => RenderedValue,
): SqlIdRefRenderer {
  return {fetch, render: render as (data: {}) => RenderedValue};
}

export const sqlIdRegistry: {[key: string]: SqlIdRefRenderer} = {};
