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
atom1.app_freeze_changed.action = 'FREEZE_APP'
atom1.app_freeze_changed.pid = 1234
atom1.app_freeze_changed.process_name = 'com.example.app'
atom1.app_freeze_changed.time_unfrozen_millis = 0
atom1.app_freeze_changed.unfreeze_reason_v2 = 'UFR_NONE'

atom2 = StatsdAtom()
atom2.app_freeze_changed.action = 'UNFREEZE_APP'
atom2.app_freeze_changed.pid = 1234
atom2.app_freeze_changed.process_name = 'com.example.app'
atom2.app_freeze_changed.time_unfrozen_millis = 4
atom2.app_freeze_changed.unfreeze_reason_v2 = 'UFR_ACTIVITY'

trace = synth_common.create_trace()
packet = trace.add_packet()
packet.statsd_atom.atom.add().ParseFromString(atom1.SerializeToString())
packet.statsd_atom.atom.add().ParseFromString(atom2.SerializeToString())
packet.statsd_atom.timestamp_nanos.extend([1000000000, 5000000000])

sys.stdout.buffer.write(trace.trace.SerializeToString())
