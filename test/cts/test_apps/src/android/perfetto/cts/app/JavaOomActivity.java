/*
 * Copyright (C) 2023 The Android Open Source Project
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

package android.perfetto.cts.app;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class JavaOomActivity extends Activity {
    public static final String TAG = "JavaOomActivity";

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(() -> {
            try {
                Log.i(TAG, "Before the allocation");
                // Try to allocate a big array: it should cause ART to run out of memory.
                byte[] alloc = new byte[Integer.MAX_VALUE];
                // Use the array, otherwise R8 might optimize the allocation away. (b/322478366,
                // b/325467497).
                alloc[5] = 42;
                Log.i(TAG, "After the allocation " + alloc[5]);
            } catch (OutOfMemoryError e) {
            }
        }).start();
    }
}
