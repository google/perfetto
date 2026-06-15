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

// Shared, schema-aware PerfettoSQL editor intelligence (completion +
// diagnostics), used by both the main UI (dev.perfetto.QueryPage) and BigTrace.
// Bind the two factories to your schema source and pass them to the Editor
// widget's `completions` / `diagnostics` props.

export type {SqlSchema, SqlSchemaTable} from './schema';
export {createPerfettoSqlCompletionSource} from './completion';
export {createPerfettoSqlDiagnosticsSource} from './diagnostics';
export {onSqlEngineReady, onSqlSchemaApplied} from './engine';
