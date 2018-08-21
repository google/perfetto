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

import '../tracks/all_controller';

import {Remote} from '../base/remote';

import {AppController} from './app_controller';
import {globals} from './globals';

function main(port: MessagePort) {
  let receivedFrontendPort = false;
  port.onmessage = ({data}) => {
    if (!receivedFrontendPort) {
      const frontendPort = data as MessagePort;
      const frontend = new Remote(frontendPort);
      globals.initialize(new AppController(), frontend);
      receivedFrontendPort = true;
    } else {
      globals.dispatch(data);
    }
  };
}

main(self as {} as MessagePort);
