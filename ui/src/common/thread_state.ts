// Copyright (C) 2019 The Android Open Source Project
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

const states: {[key: string]: string} = {
  'R': 'Runnable',
  'S': 'Sleeping',
  'D': 'Uninterruptible Sleep',
  'T': 'Stopped',
  't': 'Traced',
  'X': 'Exit (Dead)',
  'Z': 'Exit (Zombie)',
  'x': 'Task Dead',
  'I': 'Idle',
  'K': 'Wake Kill',
  'W': 'Waking',
  'P': 'Parked',
  'N': 'No Load',
  '+': '(Preempted)',
};

export function translateState(
    state: string|undefined|null, ioWait: boolean|undefined = undefined) {
  if (state === undefined) return '';
  if (state === 'Running') {
    return state;
  }
  if (state === null) {
    return 'Unknown';
  }
  let result = states[state[0]];
  if (ioWait === true) {
    result += ' (IO)';
  } else if (ioWait === false) {
    result += ' (non-IO)';
  }
  for (let i = 1; i < state.length; i++) {
    result += state[i] === '+' ? ' ' : ' + ';
    result += states[state[i]];
  }
  // state is some string we don't know how to translate.
  if (result === undefined) return state;

  return result;
}
