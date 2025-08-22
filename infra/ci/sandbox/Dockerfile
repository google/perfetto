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

# Creates an image that can check out / build / test the perfetto source.
FROM ubuntu:20.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

RUN set -ex; \
    dpkg --add-architecture i386; \
    apt-get update; \
    apt-get -y install --no-install-recommends \
        python3.9 \
        python3-pip \
        python3.9-dev \
        python3.9-venv \
        python3.9-distutils \
        python-is-python3 \
        git \
        curl \
        sudo \
        lz4 \
        tar \
        tini \
        libpulse0 \
        libgl1 \
        libxml2 \
        libc6-dev-i386 \
        linux-libc-dev:i386 \
        linux-libc-dev \
        libtinfo5 \
        gnupg2 \
        pkg-config \
        zip \
        g++ \
        zlib1g-dev \
        unzip \
        xz-utils \
        gcc-8 \
        g++-8 \
        gcc-8-multilib \
        openjdk-11-jdk \
        clang-8 \
        libc++-8-dev \
        llvm-dev \
        libc++abi-8-dev; \
    update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.9 1; \
    python3 -m pip install --no-cache-dir protobuf pandas grpcio; \
    python3 --version; \
    python --version; \
    pip3 --version; \
    gcc-8 --version; \
    g++-8 --version; \
    clang-8 --version; \
    clang++-8 --version; \
    java -version

# Chrome/puppeteer deps.
RUN set -ex; \
    apt-get update; \
    apt-get -y install --no-install-recommends \
        gconf-service \
        libasound2 \
        libatk1.0-0 \
        libc6 \
        libcairo2 \
        libcups2 \
        libdbus-1-3 \
        libexpat1 \
        libfontconfig1 \
        libgbm1 \
        libgcc1 \
        libgconf-2-4 \
        libgdk-pixbuf2.0-0 \
        libglib2.0-0 \
        libgtk-3-0 \
        libnspr4 \
        libpango-1.0-0 \
        libpangocairo-1.0-0 \
        libstdc++6 \
        libx11-6 \
        libx11-xcb1 \
        libxcb1 \
        libxcomposite1 \
        libxcursor1 \
        libxdamage1 \
        libxext6 \
        libxfixes3 \
        libxi6 \
        libxrandr2 \
        libxrender1 \
        libxss1 \
        libxtst6 \
        ca-certificates \
        libappindicator3-1 \
        libnss3 \
        lsb-release \
        xdg-utils \
        fonts-liberation \
        fonts-ipafont-gothic \
        fonts-wqy-zenhei \
        fonts-thai-tlwg \
        fonts-kacst \
        fonts-freefont-ttf;

# Additional Python utility packages
RUN set -ex; \
    apt-get update; \
    apt-get -y install --no-install-recommends \
        procps \
        jq \
        python3-jwt \
        python3-requests \
        python3-oauth2client \
        python3-httplib2 \
        python3-google-auth \
        python3-googleapi;

# Create user and group
RUN set -ex; \
    groupadd -g 1986 perfetto; \
    useradd -m -d /opt/ci -u 1986 -g perfetto -s /bin/bash perfetto; \
    # Clean up
    apt-get -y autoremove; \
    apt-get -y clean; \
    rm -rf /var/lib/apt/lists/*; \
    rm -rf /root/.cache/ /usr/share/man/* /usr/share/doc/*;

# Install gcloud
RUN curl -fsSL https://packages.cloud.google.com/apt/doc/apt-key.gpg | \
    gpg --dearmor -o /usr/share/keyrings/cloud.google.gpg; \
    echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" \
    | tee -a /etc/apt/sources.list.d/google-cloud-sdk.list; \
    apt-get update && apt-get install -y google-cloud-cli;

# Install ccache from github, the version in ubunut repo (3.7) is too old for us.
RUN set -e; \
    CCACHE_ARCHIVE_SHA256=7766991b91b3a5a177ab33fa043fe09e72c68586d5a86d20a563a05b74f119c0; \
    CCACHE_VER=4.11.3; \
    CCACHE_ARTIFACT_NAME=ccache-${CCACHE_VER}-linux-x86_64; \
    curl -fsSL -O https://github.com/ccache/ccache/releases/download/v${CCACHE_VER}/${CCACHE_ARTIFACT_NAME}.tar.xz; \
    echo "Downloaded ccache-${CCACHE_VER}."; \
    DOWNLOADED_SHA256=$(sha256sum ${CCACHE_ARTIFACT_NAME}.tar.xz | awk '{print $1}'); \
    [ "$CCACHE_ARCHIVE_SHA256" = "$DOWNLOADED_SHA256" ] || { echo "Error: sha256 do not match."; exit 1; }; \
    tar -xf ${CCACHE_ARTIFACT_NAME}.tar.xz --one-top-level; \
    cp ${CCACHE_ARTIFACT_NAME}/ccache /usr/local/bin/; \
    ccache --version; \
    rm -rf ${CCACHE_ARTIFACT_NAME} ${CCACHE_ARTIFACT_NAME}.tar.xz;

WORKDIR /opt/ci

COPY sandbox_entrypoint.sh ./
COPY tmp/config.py ./

# Download GitHub Actions runner.
# We should upkeep GHAR_VER with the latest version in
# https://api.github.com/repos/actions/runner/releases/latest
# Failing to do so will slow down every sandbox execution, as the script
# self-updates if it detects it's too old.
RUN set -ex; \
    chmod 755 ./*; \
    mkdir github-action-runner; \
    cd github-action-runner; \
    # As of May 2024, a recent version. Please check for the latest.
    # For example, on May 8 2025, 2.323.0 was a version.
    # You should update GHAR_VER to the latest from:
    # https://api.github.com/repos/actions/runner/releases/latest
    # Example: response.tag_name gives "v2.317.0", so GHAR_VER would be "2.317.0"
    GHAR_VER=${GHAR_VER:-2.323.0}; \
    GHAR_URL=https://github.com/actions/runner/releases/download/v${GHAR_VER}/actions-runner-linux-x64-${GHAR_VER}.tar.gz; \
    echo "Downloading GitHub Actions Runner version ${GHAR_VER} from ${GHAR_URL}"; \
    curl -L -o actions-runner.tar.gz $GHAR_URL; \
    tar -xzf actions-runner.tar.gz; \
    rm actions-runner.tar.gz; \
    chown -R perfetto:perfetto .;

USER perfetto
ENTRYPOINT [ "tini", "-g", "--" ]
CMD [ "bash", "/opt/ci/sandbox_entrypoint.sh" ]
