/*
 * Copyright (C) 2019 The Android Open Source Project
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

// - Protozero targets contain only headers.
// - The protozero_library() GN template invokes proto_library().
// - In chromium, proto_library() generates a static_library target in
//   non-component build configurations.
// - On some platforms (Mac, at leadst), libtool fails if the cmdline invocation
//   has no sources.
// - This file is added to each protozero target to keep libtool happy in
//   chromium.
