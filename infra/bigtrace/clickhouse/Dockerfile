# Copyright (C) 2024 The Android Open Source Project
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

# Install Python and necessary libraries for gRPC on top of Clickhouse image

FROM clickhouse/clickhouse-server

RUN apt update && apt install python3 pip curl git -y
RUN pip install grpcio grpcio-tools protobuf perfetto==0.7.0 pandas numpy

WORKDIR /tmp
RUN git clone --depth 1 https://android.googlesource.com/platform/external/perfetto/
WORKDIR /tmp/perfetto
RUN tools/install-build-deps --grpc
RUN tools/gn gen out/dist '--args=is_clang=true enable_perfetto_grpc=true'
RUN tools/ninja -C out/dist trace_processor_shell
RUN tools/gen_clickhouse_bigtrace_protos.py

RUN mkdir /var/lib/perfetto-clickhouse/
RUN cp /tmp/perfetto/out/dist/trace_processor_shell /var/lib/perfetto-clickhouse/
RUN cp -r /tmp/perfetto/python/perfetto/bigtrace_clickhouse/. /var/lib/perfetto-clickhouse/
RUN cp -r /tmp/perfetto/python/perfetto/bigtrace_clickhouse/protos/ /var/lib/perfetto-clickhouse/