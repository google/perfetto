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

import {AppImpl} from './app_impl';

// Registered by the dev.perfetto.MultiTraceOpen plugin; referenced by id to
// avoid a core -> plugin import.
const OPEN_MULTIPLE_TRACES_COMMAND =
  'dev.perfetto.MultiTraceOpen#openMultipleTraces';

// Opens one or more trace files: a single file loads directly, several files
// go through the multi-trace merge dialog.
export function openTraceFiles(files: ReadonlyArray<File>) {
  const app = AppImpl.instance;
  if (
    files.length > 1 &&
    app.commands.hasCommand(OPEN_MULTIPLE_TRACES_COMMAND)
  ) {
    app.commands.runCommand(OPEN_MULTIPLE_TRACES_COMMAND, files);
  } else if (files.length > 0) {
    app.openTraceFromFile(files[0]);
  }
}
