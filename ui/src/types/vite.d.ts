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

// Pulls in Vite's ambient type declarations so tsc accepts side-effect imports
// of *.scss / *.css and asset paths (*.png, *.svg, ...) handled by Vite's
// build pipeline, plus types for import.meta.env and import.meta.hot.
//
// We use a /// reference rather than tsconfig's "types" field because the
// latter is exclusive: setting it would disable auto-inclusion of every other
// @types/* package (node, jest, chrome, mithril, ...) and break the build.
/// <reference types="vite/client" />
