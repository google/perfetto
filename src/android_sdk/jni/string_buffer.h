/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_ANDROID_SDK_JNI_STRING_BUFFER_H_
#define SRC_ANDROID_SDK_JNI_STRING_BUFFER_H_

#include <jni.h>

#include <algorithm>
#include <list>
#include <string>
#include <string_view>

namespace perfetto {
namespace jni {

/**
 * @brief A thread-safe utility class for converting Java UTF-16 strings to
 * ASCII in a JNI environment.
 *
 * StringBuffer provides efficient conversion of Java strings to ASCII with
 * optimized memory handling. It uses a two-tiered buffering strategy:
 * 1. A fast path using pre-allocated thread-local buffers for strings up to 128
 * characters.
 * 2. A fallback path using dynamic allocation for longer strings.
 *
 * Non-ASCII characters (>255) are replaced with '?' during conversion. The
 * class maintains thread safety through thread-local storage and provides
 * zero-copy string views for optimal performance.
 *
 * Crucially, conversions allocate no per-call native memory in the common case,
 * unlike GetStringUTFChars/ScopedUtfChars. All views returned within a single
 * trace event remain valid until reset() is called (typically right after the
 * event is emitted).
 *
 * Thread Safety: All methods are thread-safe due to thread-local storage.
 */
class StringBuffer {
 private:
  static constexpr size_t BASE_SIZE = 128;
  // Temporarily stores the UTF-16 characters retrieved from the Java
  // string before they are converted to ASCII.
  static thread_local inline char char_buffer[BASE_SIZE];
  // For fast-path conversions when the resulting ASCII string fits within
  // the pre-allocated space. All ascii strings in a trace event will be stored
  // here until emitted.
  static thread_local inline jchar jchar_buffer[BASE_SIZE];
  // When the fast-path conversion is not possible (because char_buffer
  // doesn't have enough space), the converted ASCII string is stored
  // in this list. We use list here to avoid moving the strings on resize
  // with vector. This way, we can give out string_views from the stored
  // strings. The additional overhead from list node allocations is fine cos we
  // are already in an extremely unlikely path here and there are other bigger
  // problems if here.
  static thread_local inline std::list<std::string> overflow_strings;
  // current offset into the char_buffer.
  static thread_local inline size_t current_offset{0};
  // This allows us avoid touching the overflow_strings directly in the fast
  // path. Touching it causes some thread local init routine to run which shows
  // up in profiles.
  static thread_local inline bool is_overflow_strings_empty = true;

  static void copy_utf16_to_ascii(const jchar* src,
                                  size_t len,
                                  char* dst,
                                  JNIEnv* env,
                                  jstring str) {
    std::transform(src, src + len, dst, [](jchar c) {
      return (c <= 0xFF) ? static_cast<char>(c) : '?';
    });

    if (src != jchar_buffer) {
      // We hit the slow path to populate src, so we have to release.
      env->ReleaseStringCritical(str, src);
    }
  }

 public:
  static void reset() {
    if (!is_overflow_strings_empty) {
      overflow_strings.clear();
      is_overflow_strings_empty = true;
    }
    current_offset = 0;
  }

  // Converts a Java string (jstring) to an ASCII string_view. Characters
  // outside the ASCII range (0-255) are replaced with '?'.
  //
  // @param env The JNI environment.
  // @param val The Java string to convert.
  // @return A string_view representing the ASCII version of the string.
  //         Returns an empty string_view if the input is null or empty.
  static std::string_view utf16_to_ascii(JNIEnv* env, jstring val) {
    if (!val)
      return "";

    const jsize len = env->GetStringLength(val);
    if (len == 0)
      return "";

    const jchar* temp_buffer;

    // Fast path: Enough space in jchar_buffer
    if (static_cast<size_t>(len) <= BASE_SIZE) {
      env->GetStringRegion(val, 0, len, jchar_buffer);
      temp_buffer = jchar_buffer;
    } else {
      // Slow path: Fallback to asking ART for the string which will likely
      // allocate and return a copy.
      temp_buffer = env->GetStringCritical(val, nullptr);
    }

    const size_t next_offset = current_offset + static_cast<size_t>(len) + 1;
    // Fast path: Enough space in char_buffer
    if (BASE_SIZE > next_offset) {
      copy_utf16_to_ascii(temp_buffer, static_cast<size_t>(len),
                          char_buffer + current_offset, env, val);
      char_buffer[current_offset + static_cast<size_t>(len)] = '\0';

      auto res =
          std::string_view(char_buffer + current_offset, static_cast<size_t>(len));
      current_offset = next_offset;
      return res;
    } else {
      // Slow path: Not enough space in char_buffer. Use overflow_strings.
      // This will cause a string alloc but should be very unlikely to hit.
      std::string& str =
          overflow_strings.emplace_back(static_cast<size_t>(len) + 1, '\0');

      copy_utf16_to_ascii(temp_buffer, static_cast<size_t>(len), str.data(), env,
                          val);
      is_overflow_strings_empty = false;
      return std::string_view(str);
    }
  }
};

}  // namespace jni
}  // namespace perfetto

#endif  // SRC_ANDROID_SDK_JNI_STRING_BUFFER_H_
