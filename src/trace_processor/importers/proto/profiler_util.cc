/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/proto/profiler_util.h"

#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {
namespace {

base::Optional<base::StringView> PackageFromApp(base::StringView location) {
  location = location.substr(base::StringView("/data/app/").size());
  size_t slash = location.find('/');
  if (slash == std::string::npos) {
    return base::nullopt;
  }
  size_t second_slash = location.find('/', slash + 1);
  if (second_slash == std::string::npos) {
    location = location.substr(0, slash);
  } else {
    location = location.substr(slash + 1, second_slash - slash);
  }
  size_t minus = location.find('-');
  if (minus == std::string::npos) {
    return base::nullopt;
  }
  return location.substr(0, minus);
}

}  // namespace

base::Optional<std::string> PackageFromLocation(TraceStorage* storage,
                                                base::StringView location) {
  // List of some hardcoded apps that do not follow the scheme used in
  // PackageFromApp. Ask for yours to be added.
  //
  // TODO(b/153632336): Get rid of the hardcoded list of system apps.
  base::StringView sysui(
      "/system_ext/priv-app/SystemUIGoogle/SystemUIGoogle.apk");
  if (location.size() >= sysui.size() &&
      location.substr(0, sysui.size()) == sysui) {
    return "com.android.systemui";
  }

  base::StringView phonesky("/product/priv-app/Phonesky/Phonesky.apk");
  if (location.size() >= phonesky.size() &&
      location.substr(0, phonesky.size()) == phonesky) {
    return "com.android.vending";
  }

  base::StringView maps("/product/app/Maps/Maps.apk");
  if (location.size() >= maps.size() &&
      location.substr(0, maps.size()) == maps) {
    return "com.google.android.apps.maps";
  }

  base::StringView launcher(
      "/system_ext/priv-app/NexusLauncherRelease/NexusLauncherRelease.apk");
  if (location.size() >= launcher.size() &&
      location.substr(0, launcher.size()) == launcher) {
    return "com.google.android.apps.nexuslauncher";
  }

  base::StringView photos("/product/app/Photos/Photos.apk");
  if (location.size() >= photos.size() &&
      location.substr(0, photos.size()) == photos) {
    return "com.google.android.apps.photos";
  }

  base::StringView wellbeing(
      "/product/priv-app/WellbeingPrebuilt/WellbeingPrebuilt.apk");
  if (location.size() >= wellbeing.size() &&
      location.substr(0, wellbeing.size()) == wellbeing) {
    return "com.google.android.apps.wellbeing";
  }

  base::StringView matchmaker("MatchMaker");
  if (location.size() >= matchmaker.size() &&
      location.find(matchmaker) != base::StringView::npos) {
    return "com.google.android.as";
  }

  base::StringView gm("/product/app/PrebuiltGmail/PrebuiltGmail.apk");
  if (location.size() >= gm.size() && location.substr(0, gm.size()) == gm) {
    return "com.google.android.gm";
  }

  base::StringView gmscore("/product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore");
  if (location.size() >= gmscore.size() &&
      location.substr(0, gmscore.size()) == gmscore) {
    return "com.google.android.gms";
  }

  base::StringView velvet("/product/priv-app/Velvet/Velvet.apk");
  if (location.size() >= velvet.size() &&
      location.substr(0, velvet.size()) == velvet) {
    return "com.google.android.googlequicksearchbox";
  }

  base::StringView inputmethod(
      "/product/app/LatinIMEGooglePrebuilt/LatinIMEGooglePrebuilt.apk");
  if (location.size() >= inputmethod.size() &&
      location.substr(0, inputmethod.size()) == inputmethod) {
    return "com.google.android.inputmethod.latin";
  }

  base::StringView messaging("/product/app/PrebuiltBugle/PrebuiltBugle.apk");
  if (location.size() >= messaging.size() &&
      location.substr(0, messaging.size()) == messaging) {
    return "com.google.android.apps.messaging";
  }

  base::StringView data_app("/data/app/");
  if (location.substr(0, data_app.size()) == data_app) {
    auto package = PackageFromApp(location);
    if (!package) {
      PERFETTO_DLOG("Failed to parse %s", location.ToStdString().c_str());
      storage->IncrementStats(stats::deobfuscate_location_parse_error);
      return base::nullopt;
    }
    return package->ToStdString();
  }
  return base::nullopt;
}

std::string FullyQualifiedDeobfuscatedName(
    protos::pbzero::ObfuscatedClass::Decoder& cls,
    protos::pbzero::ObfuscatedMember::Decoder& member) {
  std::string member_deobfuscated_name =
      member.deobfuscated_name().ToStdString();
  if (member_deobfuscated_name.find('.') == std::string::npos) {
    // Name relative to class.
    return cls.deobfuscated_name().ToStdString() + "." +
           member_deobfuscated_name;
  } else {
    // Fully qualified name.
    return member_deobfuscated_name;
  }
}

}  // namespace trace_processor
}  // namespace perfetto
