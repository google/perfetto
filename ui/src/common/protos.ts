// Copyright (C) 2018 The Android Open Source Project
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

import * as protos from '../gen/protos';

// Aliases protos to avoid the super nested namespaces.
// See https://www.typescriptlang.org/docs/handbook/namespaces.html#aliases
import TraceConfig = protos.perfetto.protos.TraceConfig;
import TraceProcessor = protos.perfetto.protos.TraceProcessor;
import IRawQueryArgs = protos.perfetto.protos.IRawQueryArgs;
import RawQueryArgs = protos.perfetto.protos.RawQueryArgs;
import RawQueryResult = protos.perfetto.protos.RawQueryResult;

export {
  TraceConfig,
  TraceProcessor,
  IRawQueryArgs,
  RawQueryArgs,
  RawQueryResult,
};
