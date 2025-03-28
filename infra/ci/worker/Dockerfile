# Copyright (C) 2019 The Android Open Source Project
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

FROM debian:bookworm

RUN set -ex; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get update; \
    apt-get -y install \
            ca-certificates curl gnupg lsb-release supervisor sudo tini \
            python3 python3-jwt python3-requests python3-oauth2client \
            python3-httplib2 python3-google-auth python3-google-auth-oauthlib \
            python3-googleapi iptables

# Install docker (client only)
RUN curl -fsSL https://download.docker.com/linux/debian/gpg | \
    gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg; \
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] \
    https://download.docker.com/linux/debian $(lsb_release -cs) stable" | \
    tee /etc/apt/sources.list.d/docker.list > /dev/null; \
    apt-get update && apt-get install -y docker-ce-cli


# Install gcloud
RUN curl -fsSL https://packages.cloud.google.com/apt/doc/apt-key.gpg | \
    gpg --dearmor -o /usr/share/keyrings/cloud.google.gpg; \
    echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" \
    | tee -a /etc/apt/sources.list.d/google-cloud-sdk.list; \
    apt-get update && apt-get install -y google-cloud-cli

# Unfortunately Docker doesn't allow to copy a file from ../. So we copy instead
# the config files into tmp/ from the Makefile that runs docker build.

WORKDIR /opt/ci
COPY worker_entrypoint.sh ./
COPY tmp/config.py ./
COPY tmp/common_utils.py ./
COPY sandbox_runner.py ./


ENTRYPOINT [ "tini", "--" ]
CMD [ "/bin/bash", "worker_entrypoint.sh" ]
