# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys
import synth_common

StatsdAtom = synth_common.get_statsd_atom_message_class()

atom1 = StatsdAtom()
atom1.uid_process_state_changed.uid = 10001
atom1.uid_process_state_changed.state = 'PROCESS_STATE_FOREGROUND_SERVICE'

atom2 = StatsdAtom()
atom2.uid_process_state_changed.uid = 10001
atom2.uid_process_state_changed.state = 'PROCESS_STATE_TOP'

trace = synth_common.create_trace()
packet = trace.add_packet()
packet.statsd_atom.atom.add().ParseFromString(atom1.SerializeToString())
packet.statsd_atom.atom.add().ParseFromString(atom2.SerializeToString())
packet.statsd_atom.timestamp_nanos.extend([1000, 5000])

sys.stdout.buffer.write(trace.trace.SerializeToString())
