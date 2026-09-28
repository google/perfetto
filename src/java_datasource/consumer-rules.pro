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

# Keep all native methods declared in the Perfetto SDK
-keepclasseswithmembernames class dev.perfetto.sdk.** {
    native <methods>;
}

# Keep PerfettoDataSource and its callbacks called from JNI
-keep class dev.perfetto.sdk.PerfettoDataSource {
    public protected *;
    void onEnabledChanged(boolean);
    void onSetup(int, byte[]);
    void onStart(int);
    void onStop(int);
    void onFlush(int);
}

# Keep SDK public API classes
-keep class dev.perfetto.sdk.TraceContext {
    public protected *;
}

-keep class dev.perfetto.sdk.ProtoWriter {
    public protected *;
}

-keep class dev.perfetto.sdk.PacketBuilder {
    public protected *;
}

-keep class dev.perfetto.sdk.InternPool {
    public protected *;
}

# Dalvik optimization annotations are compileOnly / host-stubs
-dontwarn dalvik.annotation.optimization.**
