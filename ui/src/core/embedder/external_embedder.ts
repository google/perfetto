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

import {Embedder} from './embedder';

// External embedder override point. The default file ships
// EXTERNAL_EMBEDDER = undefined; create_embedder.ts falls through to the
// built-in PerfettoUiEmbedder / DefaultEmbedder ladder.
//
// Deployments that want to plug in a custom Embedder replace this file's
// contents — e.g. via a fork commit on a vendored copy of perfetto, or a
// setup-time symlink to a deployment-side file. When non-undefined, the
// exported value is used unconditionally regardless of the current
// origin. Example override:
//
//   import {Embedder} from './embedder';
//   import {MyEmbedder} from './my_embedder';
//
//   export const EXTERNAL_EMBEDDER: Embedder = new MyEmbedder();
export const EXTERNAL_EMBEDDER: Embedder | undefined = undefined;
