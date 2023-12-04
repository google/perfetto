#!/bin/sh
#
# Copyright 2023 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Setups the device before running perfetto tests. Recovers the state after the
# tests are done. The setup and the tead-down phases are distinguished via the
# first argument: [setup|teardown].

if [ "$1" != setup -a "$1" != teardown ]; then
    echo "Usage: $0 [setup|teardown]"
    exit 1
fi

# Only force persist.traced.enable=1 on automotive as other form factors
# might have to have traced enabled by default per the CDD.
if ! getprop ro.build.characteristics | grep -q automotive; then
    exit 0
fi

# b/285215697: Set the persist.traced.enable during the test.
PROP=persist.traced.enable
SAVED=/data/local/tests/persist.traced.enable.saved
if [ "$1" = setup ]; then
    if [ -n "$(getprop ${PROP})" ]; then
        getprop ${PROP} > ${SAVED}
    fi
    setprop ${PROP} "1"
elif [ "$1" = teardown ]; then
    if [ -e ${SAVED} ]; then
        setprop ${PROP} $(cat ${SAVED})
        rm ${SAVED}
    else
        setprop ${PROP} ""
    fi
fi
