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

# Creates an image that can check out / build / test the perfetto source. The
# image is used by the Kokoro continuous integration jobs, but is also suitable
# for local development.

FROM debian:buster

RUN set -ex; \
    export DEBIAN_FRONTEND=noninteractive; \
    echo deb http://archive.debian.org/debian buster-backports main > \
            /etc/apt/sources.list.d/backports.list; \
    apt-get update; \
    apt-get -y install python3 python3-pip git curl sudo lz4 tar ccache tini \
                       libpulse0 libgl1 libxml2 libc6-dev-i386 libtinfo5 \
                       gnupg2 pkg-config zip g++ zlib1g-dev unzip \
                       python3-distutils gcc-8 g++-8; \
    apt-get -y install libc++-8-dev libc++abi-8-dev clang-8; \
    update-alternatives --install /usr/bin/python python /usr/bin/python3.7 1; \
    gcc-8 --version; \
    g++-8 --version; \
    clang-8 --version; \
    clang++-8 --version; \
    pip3 install protobuf pandas grpcio; \
    groupadd -g 1337 perfetto; \
    useradd -d /ci/ramdisk -u 1337 -g perfetto perfetto; \
    apt-get -y autoremove; \
    rm -rf /var/lib/apt/lists/* /usr/share/man/* /usr/share/doc/*;

RUN set -ex; \
    curl -LO https://github.com/bazelbuild/bazel/releases/download/7.0.2/bazel-7.0.2-installer-linux-x86_64.sh; \
    chmod +x bazel-*-installer-linux-x86_64.sh; \
    ./bazel-*-installer-linux-x86_64.sh; \
    rm bazel-*-installer-linux-x86_64.sh; \
    bazel version;

# Chrome/puppeteer deps.
RUN set -ex; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get update; \
    apt-get -y install gconf-service libasound2 libatk1.0-0 libc6 libcairo2 \
               libcups2 libdbus-1-3 libexpat1 libfontconfig1 libgbm1 libgcc1 \
               libgconf-2-4 libgdk-pixbuf2.0-0 libglib2.0-0 libgtk-3-0 libnspr4 \
               libpango-1.0-0 libpangocairo-1.0-0 libstdc++6 libx11-6 \
               libx11-xcb1 libxcb1 libxcomposite1 libxcursor1 libxdamage1 \
               libxext6 libxfixes3 libxi6 libxrandr2 libxrender1 libxss1 \
               libxtst6 ca-certificates libappindicator1 libnss3 lsb-release \
               xdg-utils fonts-liberation fonts-ipafont-gothic fonts-wqy-zenhei \
               fonts-thai-tlwg fonts-kacst fonts-freefont-ttf

# Cleanup to reduce image size
RUN     apt-get -y autoremove; \
        rm -rf /var/lib/apt/lists/* /usr/share/man/* /usr/share/doc/*; \
        rm -rf /root/.cache/;

COPY testrunner.sh /ci/testrunner.sh
COPY init.sh /ci/init.sh
RUN chmod -R a+rx /ci/

VOLUME [ "/ci/cache", "/ci/ramdisk", "/ci/artifacts" ]
ENTRYPOINT [ "tini", "-g", "--" ]
CMD [ "bash", "/ci/init.sh" ]

