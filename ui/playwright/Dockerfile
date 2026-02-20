# Copyright (C) 2025 The Android Open Source Project
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

# Playwright container with Mesa GL support for headless rendering.
# Build with: docker build --build-arg PLAYWRIGHT_VERSION=$(pnpm exec playwright --version) -t perfetto-playwright .
# Use the same base as infra/ci/sandbox/Dockerfile to make it as identical to the CI as possible
FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        # Mesa/GL dependencies
        libgl1-mesa-dri \
        mesa-utils \
        xvfb \
        expect \
        python3 \
        # Node.js
        curl \
        ca-certificates \
        git && \
    curl -fsSL https://deb.nodesource.com/setup_20.x | bash - && \
    apt-get install -y nodejs && \
    npm install -g pnpm && \
    apt-get install -y --no-install-recommends \
        # Chromium dependencies
        libnss3 \
        libnspr4 \
        libatk1.0-0 \
        libatk-bridge2.0-0 \
        libcups2 \
        libdrm2 \
        libdbus-1-3 \
        libxkbcommon0 \
        libatspi2.0-0 \
        libx11-6 \
        libxcomposite1 \
        libxdamage1 \
        libxext6 \
        libxfixes3 \
        libxrandr2 \
        libgbm1 \
        libpango-1.0-0 \
        libcairo2 \
        libasound2 \
        # Fonts
        fonts-liberation \
        fonts-noto-color-emoji && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*
