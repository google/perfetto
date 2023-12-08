// Copyright (C) 2023 The Android Open Source Project
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

// Occasionally libraries require configuration code to be called
// before that library is used. This can become troublesome when the
// library is used in many places through out the code base. To ensure
// a consistent initialization this file exists as a place such code
// can live. It's the first import in the root file of the bundle.

// We use 'static initializers' (e.g. first import causes
// initialization) rather than allowing the importer control over when
// it happens. This sounds worse on paper but it works a lot better in
// tests where (in JS) there is no global main() you can edit to do the
// initialization. Instead any test where this is a problem can easily
// stick an import at the top of the file.

import {enableMapSet, enablePatches, setAutoFreeze} from 'immer';
import protobuf from 'protobufjs/minimal';

function initializeImmer() {
  enablePatches();
  enableMapSet();

  // TODO(primiano): re-enable this, requires fixing some bugs that this bubbles
  // up. This is a new feature of immer which freezes object after a produce().
  // Unfortunately we piled up a bunch of bugs where we shallow-copy objects
  // from the global state (which is frozen) and later try to update the copies.
  // By doing so, we  accidentally the local copy of global state, which is
  // supposed to be immutable.
  setAutoFreeze(true);
}

function initializeProtobuf() {
  // Disable Long.js support in protobuf. This seems to be enabled only in tests
  // but not in production code. In any case, for now we want casting to number
  // accepting the 2**53 limitation. This is consistent with passing
  // --force-number in the protobuf.js codegen invocation in //ui/BUILD.gn .
  // See also https://github.com/protobufjs/protobuf.js/issues/1253 .
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  protobuf.util.Long = undefined as any;
  protobuf.configure();
}

let isInitialized = false;
function initialize() {
  if (isInitialized) {
    throw new Error('initialize() should be called exactly once');
  }
  initializeImmer();
  initializeProtobuf();
  isInitialized = true;
}

// JS module semantics ensure this is happens only once.
initialize();
