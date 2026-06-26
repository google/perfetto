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

// BigTrace bindings for the shared PerfettoSQL editor intelligence
// (src/components/sql_intelligence). The shared completion + diagnostics logic
// is identical to the main UI's; only the schema source differs — here it's the
// BigTrace stdlib catalog (sqlTablesLoader), which satisfies the shared
// SqlSchema structurally.

import {
  createPerfettoSqlCompletionSource,
  createPerfettoSqlDiagnosticsSource,
} from '../../components/sql_intelligence';
import {sqlTablesLoader} from './sql_tables';

const getSchema = () => sqlTablesLoader.modules;

export const perfettoSqlCompletions =
  createPerfettoSqlCompletionSource(getSchema);

export const perfettoSqlDiagnostics =
  createPerfettoSqlDiagnosticsSource(getSchema);
