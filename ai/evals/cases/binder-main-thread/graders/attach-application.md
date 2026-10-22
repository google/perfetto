---
type: regex
pattern: attachApplication
---
Ground truth (android.binder android_binder_txns, client = Gmail main
thread, clipped to the cold start): IActivityManager::attachApplication
12.1 ms (1 call, system_server), 22 calls to servicemanager 8.2 ms total,
IJobScheduler::enqueue 5.9 ms (2 calls), IActivityTaskManager::startActivity
4.9 ms, IActivityManager::isUserAMonkey 3.3 ms (2 calls),
IWindowSession::relayout 3.2 ms.
