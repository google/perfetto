/*
 * Copyright (C) 2018 The Android Open Source Project
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

package android.perfetto.producer;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;

public class ProducerActivity extends Activity {
    private boolean paused = true;
    private final Handler handler = new Handler();

    @Override
    public void onResume() {
        super.onResume();

        paused = false;
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (paused) {
                    return;
                }

                startService(new Intent(ProducerActivity.this, ProducerService.class));
                startService(new Intent(ProducerActivity.this, ProducerIsolatedService.class));

                System.loadLibrary("perfettocts_jni");
                new Thread(new Runnable() {
                    public void run() {
                        try {
                            setupProducer();
                        } catch (Exception ex) {
                            ex.printStackTrace();
                        }
                    }
                })
                        .start();
            }
        });
    }

    @Override
    public void onPause() {
        super.onPause();
        paused = true;
    }

    private static native void setupProducer();
}
