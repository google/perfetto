#!/usr/bin/env lucicfg
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

lucicfg.check_version("1.30.9", "Please update depot_tools")

# Use LUCI Scheduler BBv2 names and add Scheduler realms configs.
lucicfg.enable_experiment("crbug.com/1182002")

# Enable bbagent.
luci.recipe.defaults.use_bbagent.set(True)

lucicfg.config(
    config_dir = "generated",
    fail_on_warnings = True,
)

luci.project(
    name = "perfetto",
    buildbucket = "cr-buildbucket.appspot.com",
    logdog = "luci-logdog",
    milo = "luci-milo",
    scheduler = "luci-scheduler",
    swarming = "chrome-swarming.appspot.com",
    acls = [
        acl.entry(
            [
                acl.BUILDBUCKET_READER,
                acl.LOGDOG_READER,
                acl.PROJECT_CONFIGS_READER,
                acl.SCHEDULER_READER,
            ],
            groups = ["all"],
        ),
        acl.entry(roles = acl.SCHEDULER_OWNER, groups = "mdb/perfetto-cloud-infra"),
        acl.entry([acl.LOGDOG_WRITER], groups = ["luci-logdog-chromium-writers"]),
    ],
)

# Use the default Chromium logdog instance as:
# a) we expect our logs to be very minimal
# b) we are open source so there's nothing special in our logs.
luci.logdog(
    gs_bucket = "chromium-luci-logdog",
)

# Create a realm for the official pool.
# Used by LUCI infra (Googlers: see pools.cfg) to enforce ACLs.
luci.realm(name = "pools/official")

# Bucket used by all official builders.
luci.bucket(
    name = "official",
    acls = [
        acl.entry(
            roles = [acl.BUILDBUCKET_TRIGGERER],
            groups = ["mdb/perfetto-cloud-infra"],
        ),
        acl.entry(
            roles = [acl.SCHEDULER_TRIGGERER, acl.BUILDBUCKET_TRIGGERER],
            groups = ["mdb/chrome-troopers"],
        ),
    ],
)

def official_builder(name, os, caches=[]):
    luci.builder(
        name = name,
        bucket = "official",
        executable = luci.recipe(
            name = "perfetto",
            cipd_package = "infra/recipe_bundles/android.googlesource.com/platform/external/perfetto",
            cipd_version = "refs/heads/master",
            use_python3 = True,
        ),
        dimensions = {
            "pool": "luci.perfetto.official",
            "os": os,
            "cpu": "x86-64",
        },
        service_account = "perfetto-luci-official-builder@chops-service-accounts.iam.gserviceaccount.com",
        triggered_by = [
            luci.gitiles_poller(
                name = "perfetto-gitiles-trigger",
                bucket = "official",
                repo = "https://android.googlesource.com/platform/external/perfetto",
                refs = ["refs/tags/v.+"],
            ),
        ],
        caches = [
            swarming.cache(cache, name = cache)
            for cache in caches
        ]
    )

official_builder("perfetto-official-builder-linux", "Linux")
official_builder("perfetto-official-builder-mac", "Mac", ["macos_sdk"])
official_builder("perfetto-official-builder-windows", "Windows", ["windows_sdk"])
official_builder("perfetto-official-builder-android", "Linux")
