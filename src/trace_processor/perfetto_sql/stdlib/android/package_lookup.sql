--
-- Copyright 2025 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

CREATE PERFETTO TABLE _package_lookup_source AS
WITH
  system_packages(uid, package_name) AS (
    SELECT
      *
    FROM (VALUES
      (0, "AID_ROOT"),
      (1000, "AID_SYSTEM_USER"),
      (1001, "AID_RADIO"),
      (1002, "AID_BLUETOOTH"),
      (1003, "AID_GRAPHICS"),
      (1004, "AID_INPUT"),
      (1005, "AID_AUDIO"),
      (1006, "AID_CAMERA"),
      (1007, "AID_LOG"),
      (1008, "AID_COMPASS"),
      (1009, "AID_MOUNT"),
      (1010, "AID_WIFI"),
      (1011, "AID_ADB"),
      (1012, "AID_INSTALL"),
      (1013, "AID_MEDIA"),
      (1014, "AID_DHCP"),
      (1015, "AID_SDCARD_RW"),
      (1016, "AID_VPN"),
      (1017, "AID_KEYSTORE"),
      (1018, "AID_USB"),
      (1019, "AID_DRM"),
      (1020, "AID_MDNSR"),
      (1021, "AID_GPS"),
      (1022, "AID_UNUSED1"),
      (1023, "AID_MEDIA_RW"),
      (1024, "AID_MTP"),
      (1025, "AID_UNUSED2"),
      (1026, "AID_DRMRPC"),
      (1027, "AID_NFC"),
      (1028, "AID_SDCARD_R"),
      (1029, "AID_CLAT"),
      (1030, "AID_LOOP_RADIO"),
      (1031, "AID_MEDIA_DRM"),
      (1032, "AID_PACKAGE_INFO"),
      (1033, "AID_SDCARD_PICS"),
      (1034, "AID_SDCARD_AV"),
      (1035, "AID_SDCARD_ALL"),
      (1036, "AID_LOGD"),
      (1037, "AID_SHARED_RELRO"),
      (1038, "AID_DBUS"),
      (1039, "AID_TLSDATE"),
      (1040, "AID_MEDIA_EX"),
      (1041, "AID_AUDIOSERVER"),
      (1042, "AID_METRICS_COLL"),
      (1043, "AID_METRICSD"),
      (1044, "AID_WEBSERV"),
      (1045, "AID_DEBUGGERD"),
      (1046, "AID_MEDIA_CODEC"),
      (1047, "AID_CAMERASERVER"),
      (1048, "AID_FIREWALL"),
      (1049, "AID_TRUNKS"),
      (1050, "AID_NVRAM"),
      (1051, "AID_DNS"),
      (1052, "AID_DNS_TETHER"),
      (1053, "AID_WEBVIEW_ZYGOTE"),
      (1054, "AID_VEHICLE_NETWORK"),
      (1055, "AID_MEDIA_AUDIO"),
      (1056, "AID_MEDIA_VIDEO"),
      (1057, "AID_MEDIA_IMAGE"),
      (1058, "AID_TOMBSTONED"),
      (1059, "AID_MEDIA_OBB"),
      (1060, "AID_ESE"),
      (1061, "AID_OTA_UPDATE"),
      (1062, "AID_AUTOMOTIVE_EVS"),
      (1063, "AID_LOWPAN"),
      (1064, "AID_HSM"),
      (1065, "AID_RESERVED_DISK"),
      (1066, "AID_STATSD"),
      (1067, "AID_INCIDENTD"),
      (1068, "AID_SECURE_ELEMENT"),
      (1069, "AID_LMKD"),
      (1070, "AID_LLKD"),
      (1071, "AID_IORAPD"),
      (1072, "AID_GPU_SERVICE"),
      (1073, "AID_NETWORK_STACK"),
      (1074, "AID_GSID"),
      (1075, "AID_FSVERITY_CERT"),
      (1076, "AID_CREDSTORE"),
      (1077, "AID_EXTERNAL_STORAGE"),
      (1078, "AID_EXT_DATA_RW"),
      (1079, "AID_EXT_OBB_RW"),
      (1080, "AID_CONTEXT_HUB"),
      (1081, "AID_VIRTMANAGER"),
      (1082, "AID_ARTD"),
      (1083, "AID_UWB"),
      (1084, "AID_THREAD_NETWORK"),
      (1085, "AID_DICED"),
      (1086, "AID_DMESGD"),
      (1087, "AID_JC_WEAVER"),
      (1088, "AID_JC_STRONGBOX"),
      (1089, "AID_JC_IDENTITYCRED"),
      (1090, "AID_SDK_SANDBOX"),
      (1091, "AID_SECURITY_LOG_WRITER"),
      (1092, "AID_PRNG_SEEDER"),
      (1093, "AID_UPROBESTATS"),
      (2000, "AID_SHELL"),
      (2001, "AID_CACHE"),
      (2002, "AID_DIAG"),
      (9999, "AID_NOBODY")) AS _values
  ),
  scored_packages AS (
    SELECT
      uid,
      package_name,
      CASE
        -- Prefer GMS core over its various aliases.
        WHEN package_name GLOB 'com.google.android.gms*'
        THEN 0
        -- Some APKs have a load of providers listed, these are less interesting.
        WHEN package_name GLOB 'com.android.providers.*'
        THEN 2
        ELSE 1
      END AS score
    FROM package_list
    WHERE
      uid >= 10000
  ),
  ranked_packages AS (
    SELECT
      uid,
      package_name,
      row_number() OVER (PARTITION BY uid ORDER BY score, package_name) AS rank
    FROM scored_packages
  )
SELECT
  uid,
  package_name
FROM system_packages
UNION ALL
SELECT
  uid,
  package_name
FROM ranked_packages
WHERE
  rank = 1;

CREATE PERFETTO FUNCTION _resolve_package_name(
    lookup_result STRING,
    pkgid LONG,
    uid LONG
)
RETURNS STRING AS
SELECT
  CASE
    WHEN $lookup_result IS NOT NULL
    THEN $lookup_result
    WHEN $pkgid >= 50000 AND $pkgid < 60000
    THEN 'SHARED_GID'
    WHEN $pkgid >= 90000
    THEN 'ISOLATED_UID'
    ELSE 'uid=' || $uid
  END;

-- Resolves the Android package name using the `uid` column from the source
-- table. The output contains all input columns, plus a `package_name` column.
CREATE PERFETTO MACRO android_package_lookup(
    -- A table or subquery containing a `uid` column for package lookup.
    src TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  SELECT
    base.*,
    _resolve_package_name(pkg.package_name, base.uid % 100000, base.uid) AS package_name
  FROM $src AS base
  LEFT JOIN _package_lookup_source AS pkg
    ON (
      pkg.uid = base.uid % 100000
    )
);
