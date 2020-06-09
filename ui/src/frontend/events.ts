// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// layerX and layerY aren't standardized but there is no drop-in replacement
// (offsetX/offsetY have slightly different semantics) and they are still
// present in the browsers we care about, so for now we create an extended
// version of MouseEvent we can use instead.
// See also:
// https://github.com/microsoft/TypeScript/issues/35634#issuecomment-564765179
export interface PerfettoMouseEvent extends MouseEvent {
  layerX: number;
  layerY: number;
}
