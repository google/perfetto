/*
 * Copyright (C) 2021 The Android Open Source Project
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

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Scanner;
import java.util.TreeMap;

public class JavaAllocActivity extends Activity {
    // Keep in sync with heapprofd_test_cts.cc
    private static final String CYCLE_REPORT_PATH = "report_cycle.txt";

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);

        new Thread(new Runnable() {
            public void run() {
                try {
                    runAllocationLoop(getExternalFilesDir(null));
                } catch (Exception ex) {
                    ex.printStackTrace();
                }
            }
        }).start();
    }

    private static TreeMap treeMap = new TreeMap();
    private static long index = 0;

    private static void runAllocationLoop(File external) throws IOException {
        for (;;) {
            for (int i = 0; i < 2000; i++) {
                Object o = new Object();
                treeMap.put(++index, o);
            }
            reportCycle(external);
            try {
                Thread.sleep(10);
            } catch (InterruptedException ignored) {
            }
            treeMap.clear();
        }
    }

    // Increments a value in a file in the app `external` directory. The file is read by the CTS
    // test to observe the app progress.
    private static void reportCycle(File external) throws IOException {
        File f = new File(external, CYCLE_REPORT_PATH);
        File tmp = new File(external, CYCLE_REPORT_PATH + ".tmp");
        long val = 0;
        // Read the previous value from the file (it might be from a separate execution of this
        // app).
        try (Scanner scanner = new Scanner(f)) {
            if (scanner.hasNextLong()) {
                val = scanner.nextLong();
            }
        } catch (FileNotFoundException ignored) {
        }

        try (FileWriter wr = new FileWriter(tmp)) {
            wr.write(Long.toString(val + 1));
        }

        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING,
                StandardCopyOption.ATOMIC_MOVE);
    }
}
