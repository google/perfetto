// Copyright (C) 2021 The Android Open Source Project
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

import {enableMapSet, enablePatches, setAutoFreeze} from 'immer';

export function initializeImmerJs() {
  enablePatches();

  // TODO(primiano): re-enable this, requires fixing some bugs that this bubbles
  // up. This is a new feature of immer which freezes object after a produce().
  // Unfortunately we piled up a bunch of bugs where we shallow-copy objects
  // from the global state (which is frozen) and later try to update the copies.
  // By doing so, we  accidentally the local copy of global state, which is
  // supposed to be immutable.
  setAutoFreeze(false);

  enableMapSet();
}
