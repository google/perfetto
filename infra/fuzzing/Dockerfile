# Copyright (C) 2018 The Android Open Source Project
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

# Build with
# cd infra/fuzzing
# $ sudo docker build -t fuzzing .

# Run with
# $ CONTAINER=$(sudo docker run -d --cap-add=SYS_PTRACE fuzzing)

# Get outputs with
# $ sudo docker cp -a ${CONTAINER}:/home/perfetto/perfetto/fuzz_out /tmp

FROM debian:latest

ENV DEBIAN_FRONTEND noninteractive

RUN echo deb http://deb.debian.org/debian testing main > /etc/apt/sources.list.d/testing.list
RUN apt-get update
RUN apt-get -y install python git curl
# gcc-7 for sysroot
RUN apt-get -y -t testing install gcc-7

# pip for installing certiain test script dependencies
RUN curl https://bootstrap.pypa.io/get-pip.py | python -

RUN useradd -m perfetto
USER perfetto:perfetto
WORKDIR /home/perfetto

RUN git clone https://android.googlesource.com/platform/external/perfetto/

CMD cd perfetto/ && tools/continuous_fuzz
