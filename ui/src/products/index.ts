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

import type {Embedder} from '../core/embedder/embedder';
import {MemscopeEmbedder} from './memscope/memscope_embedder';

// Standalone products built from this UI. The key is the product id used by
// `ui/build --product`, `?product=` and the `<id>.perfetto.dev` hostname.
export const PRODUCTS: ReadonlyMap<string, () => Embedder> = new Map([
  ['memscope', () => new MemscopeEmbedder()],
]);
