# Copyright (C) 2021 The Android Open Source Project
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

# The image that builds the Perfetto UI and deploys to GCS.
# See go/perfetto-ui-autopush for docs on how this works end-to-end.

FROM debian:buster-slim

ENV PATH=/builder/google-cloud-sdk/bin/:$PATH
RUN set -ex; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get update; \
    apt-get -y install python3 python3-distutils python3-pip git curl tar tini \
            pkg-config zip libc-dev libgcc-8-dev; \
    update-alternatives --install /usr/bin/python python /usr/bin/python3.7 1; \
    pip3 install --quiet protobuf crcmod; \
    mkdir -p /builder && \
    curl -s -o - https://dl.google.com/dl/cloudsdk/release/google-cloud-sdk.tar.gz | tar -zx -C /builder; \
    /builder/google-cloud-sdk/install.sh \
        --usage-reporting=false \
        --bash-completion=false \
        --disable-installation-options \
        --override-components gcloud gsutil; \
    git config --system credential.helper gcloud.sh; \
    useradd -d /home/perfetto perfetto; \
    apt-get -y autoremove; \
    rm -rf /var/lib/apt/lists/* /usr/share/man/* /usr/share/doc/*;

ENTRYPOINT [ "tini", "-g", "--" ]
