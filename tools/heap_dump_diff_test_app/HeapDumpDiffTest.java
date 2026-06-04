import com.sun.management.HotSpotDiagnosticMXBean;
import java.io.File;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import javax.management.MBeanServer;

/**
 * Java app that builds a rich, layered heap (Android-app-shaped) and
 * captures two hprof snapshots — one of the "busy UI" state, one of the
 * "background-services-heavy" state.
 *
 * Usage:
 *   javac HeapDumpDiffTest.java
 *   java -Xmx512m HeapDumpDiffTest baseline.hprof current.hprof
 *
 * The app builds:
 *   Application
 *     ActivityManager
 *       Activity*
 *         FragmentManager
 *           Fragment*
 *             ViewHolder*
 *               TextView*  -> String*
 *               ImageView* -> Bitmap*
 *             ViewModel
 *               DataCache  -> CacheEntry -> byte[]
 *               ApiClient  -> HttpResponse -> byte[]
 *           BackStack -> BackStackEntry*
 *     ServiceManager
 *       BackgroundService*
 *         Worker -> Task -> TaskData -> byte[]
 *                 -> Scheduler -> ScheduledTask*
 *         EventBus -> EventListener*
 *       ConnectivityService -> ConnectionPool -> Connection -> SocketBuffer
 *
 * Between dumps, the UI shrinks (user closed activities) while background
 * services and network connections grow (long-running jobs accumulated).
 */
public class HeapDumpDiffTest {

    // Root reference holder so nothing is reclaimed before dumpHeap runs.
    static Application app;

    public static void main(String[] args) throws IOException, InterruptedException {
        if (args.length < 2) {
            System.err.println(
                    "Usage: HeapDumpDiffTest <baseline.hprof> <current.hprof>");
            System.exit(1);
        }

        // ---- Phase 1: busy UI, light background, light network ---------
        app = new Application();
        app.buildState(/*activities=*/ 4, /*fragmentsPerActivity=*/ 3,
                /*holdersPerFragment=*/ 4,
                /*textviewsPerHolder=*/ 2, /*stringsPerTextview=*/ 2,
                /*imageviewsPerHolder=*/ 1, /*bitmapsPerImageview=*/ 2,
                /*cachesPerVm=*/ 2, /*entriesPerCache=*/ 4,
                /*apisPerVm=*/ 1, /*respsPerApi=*/ 2,
                /*backstackEntries=*/ 3,
                /*bgServices=*/ 2, /*workersPerService=*/ 2,
                /*tasksPerWorker=*/ 2, /*scheduledPerScheduler=*/ 2,
                /*listenersPerEventBus=*/ 3,
                /*connections=*/ 2, /*buffersPerConnection=*/ 2);
        // Add some RemovedClass instances that go away in dump 2.
        for (int i = 0; i < 8; i++) {
            app.removed.add(new RemovedClass(i));
        }
        System.gc();
        Thread.sleep(500);
        dumpHeap(args[0]);
        System.out.println("Wrote baseline: " + args[0]);

        // ---- Phase 2: UI mostly closed, services accumulated ------------
        // Reset to a fresh graph with the second profile. Building a fresh
        // graph keeps each dump's structure clean and avoids leftover
        // sub-trees from phase 1 distorting the diff.
        app = new Application();
        app.buildState(/*activities=*/ 2, /*fragmentsPerActivity=*/ 2,
                /*holdersPerFragment=*/ 4,
                /*textviewsPerHolder=*/ 2, /*stringsPerTextview=*/ 2,
                /*imageviewsPerHolder=*/ 1, /*bitmapsPerImageview=*/ 4,
                /*cachesPerVm=*/ 1, /*entriesPerCache=*/ 2,
                /*apisPerVm=*/ 2, /*respsPerApi=*/ 4,
                /*backstackEntries=*/ 2,
                /*bgServices=*/ 3, /*workersPerService=*/ 4,
                /*tasksPerWorker=*/ 3, /*scheduledPerScheduler=*/ 4,
                /*listenersPerEventBus=*/ 5,
                /*connections=*/ 4, /*buffersPerConnection=*/ 4);
        // RemovedClass is gone; NewlyAddedClass is here.
        for (int i = 0; i < 6; i++) {
            app.newlyAdded.add(new NewlyAddedClass(i));
        }
        System.gc();
        Thread.sleep(500);
        dumpHeap(args[1]);
        System.out.println("Wrote current: " + args[1]);
    }

    static void dumpHeap(String path) throws IOException {
        File f = new File(path);
        if (f.exists() && !f.delete()) {
            throw new IOException("Could not delete existing " + path);
        }
        MBeanServer server = ManagementFactory.getPlatformMBeanServer();
        HotSpotDiagnosticMXBean bean = ManagementFactory.newPlatformMXBeanProxy(
                server,
                "com.sun.management:type=HotSpotDiagnostic",
                HotSpotDiagnosticMXBean.class);
        bean.dumpHeap(path, /*live=*/ true);
    }

    // ----- Object graph nodes --------------------------------------------
    //
    // Each class is a real Java type so it shows up in the hprof with its
    // own classloader entry. Field references hold children alive between
    // dumps so the dominator tree has interesting structure.

    static class Application {
        ActivityManager activityManager = new ActivityManager();
        ServiceManager serviceManager = new ServiceManager();
        List<RemovedClass> removed = new ArrayList<>();
        List<NewlyAddedClass> newlyAdded = new ArrayList<>();

        void buildState(int activities, int fragmentsPerActivity,
                        int holdersPerFragment, int textviewsPerHolder,
                        int stringsPerTextview, int imageviewsPerHolder,
                        int bitmapsPerImageview, int cachesPerVm,
                        int entriesPerCache, int apisPerVm, int respsPerApi,
                        int backstackEntries, int bgServices,
                        int workersPerService, int tasksPerWorker,
                        int scheduledPerScheduler, int listenersPerEventBus,
                        int connections, int buffersPerConnection) {
            for (int a = 0; a < activities; a++) {
                Activity act = new Activity("activity-" + a);
                activityManager.activities.add(act);
                for (int f = 0; f < fragmentsPerActivity; f++) {
                    Fragment frag = new Fragment("fragment-" + a + "-" + f);
                    act.fragmentManager.fragments.add(frag);
                    for (int h = 0; h < holdersPerFragment; h++) {
                        ViewHolder vh = new ViewHolder();
                        frag.holders.add(vh);
                        for (int t = 0; t < textviewsPerHolder; t++) {
                            TextView tv = new TextView();
                            vh.textViews.add(tv);
                            for (int s = 0; s < stringsPerTextview; s++) {
                                tv.strings.add(UUID.randomUUID().toString());
                            }
                        }
                        for (int i = 0; i < imageviewsPerHolder; i++) {
                            ImageView iv = new ImageView();
                            vh.imageViews.add(iv);
                            for (int b = 0; b < bitmapsPerImageview; b++) {
                                iv.bitmaps.add(new Bitmap(64 * 1024));
                            }
                        }
                    }
                    for (int c = 0; c < cachesPerVm; c++) {
                        DataCache dc = new DataCache();
                        frag.viewModel.caches.add(dc);
                        for (int e = 0; e < entriesPerCache; e++) {
                            CacheEntry ce = new CacheEntry();
                            ce.payload = new byte[8 * 1024];
                            dc.entries.add(ce);
                        }
                    }
                    for (int p = 0; p < apisPerVm; p++) {
                        ApiClient api = new ApiClient();
                        frag.viewModel.apis.add(api);
                        for (int r = 0; r < respsPerApi; r++) {
                            HttpResponse resp = new HttpResponse();
                            resp.body = new byte[16 * 1024];
                            api.responses.add(resp);
                        }
                    }
                    for (int e = 0; e < backstackEntries; e++) {
                        act.fragmentManager.backStack.entries.add(
                                new BackStackEntry("entry-" + e));
                    }
                }
            }
            for (int s = 0; s < bgServices; s++) {
                BackgroundService svc = new BackgroundService("svc-" + s);
                serviceManager.services.add(svc);
                for (int w = 0; w < workersPerService; w++) {
                    Worker worker = new Worker();
                    svc.workers.add(worker);
                    for (int t = 0; t < tasksPerWorker; t++) {
                        Task task = new Task();
                        task.data = new TaskData();
                        task.data.payload = new byte[2 * 1024];
                        worker.tasks.add(task);
                    }
                    for (int sc = 0; sc < scheduledPerScheduler; sc++) {
                        worker.scheduler.scheduled.add(
                                new ScheduledTask(sc));
                    }
                }
                for (int l = 0; l < listenersPerEventBus; l++) {
                    svc.eventBus.listeners.add(new EventListener(l));
                }
            }
            for (int c = 0; c < connections; c++) {
                Connection conn = new Connection();
                serviceManager.connectivity.connectionPool.connections.add(conn);
                for (int b = 0; b < buffersPerConnection; b++) {
                    SocketBuffer sb = new SocketBuffer();
                    sb.payload = new byte[32 * 1024];
                    conn.buffers.add(sb);
                }
            }
        }
    }

    static class ActivityManager {
        List<Activity> activities = new ArrayList<>();
    }

    static class Activity {
        String name;
        FragmentManager fragmentManager = new FragmentManager();
        Activity(String n) { this.name = n; }
    }

    static class FragmentManager {
        List<Fragment> fragments = new ArrayList<>();
        BackStack backStack = new BackStack();
    }

    static class Fragment {
        String tag;
        List<ViewHolder> holders = new ArrayList<>();
        ViewModel viewModel = new ViewModel();
        Fragment(String t) { this.tag = t; }
    }

    static class BackStack {
        List<BackStackEntry> entries = new ArrayList<>();
    }

    static class BackStackEntry {
        String name;
        BackStackEntry(String n) { this.name = n; }
    }

    static class ViewHolder {
        List<TextView> textViews = new ArrayList<>();
        List<ImageView> imageViews = new ArrayList<>();
    }

    static class TextView {
        List<String> strings = new ArrayList<>();
    }

    static class ImageView {
        List<Bitmap> bitmaps = new ArrayList<>();
    }

    static class Bitmap {
        byte[] pixels;
        Bitmap(int n) { this.pixels = new byte[n]; }
    }

    static class ViewModel {
        List<DataCache> caches = new ArrayList<>();
        List<ApiClient> apis = new ArrayList<>();
    }

    static class DataCache {
        List<CacheEntry> entries = new ArrayList<>();
    }

    static class CacheEntry {
        byte[] payload;
    }

    static class ApiClient {
        List<HttpResponse> responses = new ArrayList<>();
    }

    static class HttpResponse {
        byte[] body;
    }

    static class ServiceManager {
        List<BackgroundService> services = new ArrayList<>();
        ConnectivityService connectivity = new ConnectivityService();
    }

    static class BackgroundService {
        String name;
        List<Worker> workers = new ArrayList<>();
        EventBus eventBus = new EventBus();
        BackgroundService(String n) { this.name = n; }
    }

    static class Worker {
        List<Task> tasks = new ArrayList<>();
        Scheduler scheduler = new Scheduler();
    }

    static class Task {
        TaskData data;
    }

    static class TaskData {
        byte[] payload;
    }

    static class Scheduler {
        List<ScheduledTask> scheduled = new ArrayList<>();
    }

    static class ScheduledTask {
        int delayMs;
        ScheduledTask(int s) { this.delayMs = s; }
    }

    static class EventBus {
        List<EventListener> listeners = new ArrayList<>();
    }

    static class EventListener {
        int id;
        EventListener(int i) { this.id = i; }
    }

    static class ConnectivityService {
        ConnectionPool connectionPool = new ConnectionPool();
    }

    static class ConnectionPool {
        List<Connection> connections = new ArrayList<>();
    }

    static class Connection {
        List<SocketBuffer> buffers = new ArrayList<>();
    }

    static class SocketBuffer {
        byte[] payload;
    }

    static class RemovedClass {
        int seed;
        RemovedClass(int s) { this.seed = s; }
    }

    static class NewlyAddedClass {
        int seed;
        NewlyAddedClass(int s) { this.seed = s; }
    }
}
