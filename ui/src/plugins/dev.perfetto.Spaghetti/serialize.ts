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

// Serializes the graph upstream of an output node into one query, in Standard
// SQL or PerfettoSQL Next (PSN).
//
// Knows nothing about planning, chunks or caching: it renders whatever is
// upstream of `outputId`, stopping at nodes the caller names as tables.

import type {GraphIndex} from './graph_utils';
import type {SerializeOptions, SerializeResult} from './serialize_common';
import {serializePsn} from './serialize_psn';
import {serializeSql} from './serialize_sql';
import type {SqlDialect} from './sql_types';

/** Serializes everything upstream of `outputId` as one query in `dialect`. */
export function serialize(
  index: GraphIndex,
  outputId: string,
  dialect: SqlDialect,
  options: SerializeOptions = {},
): SerializeResult {
  return dialect === 'pipe'
    ? serializePsn(index, outputId, options)
    : serializeSql(index, outputId, options);
}
