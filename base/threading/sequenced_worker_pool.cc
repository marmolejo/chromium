// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/sequenced_worker_pool.h"

#include <list>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "base/atomic_sequence_num.h"
#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/memory/linked_ptr.h"
#include "base/message_loop/message_loop_proxy.h"
#include "base/stl_util.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/threading/simple_thread.h"
#include "base/threading/thread_local.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"

#if defined(OS_MACOSX)
#include "base/mac/scoped_nsautorelease_pool.h"
#elif defined(OS_WIN)
#include "base/win/scoped_com_initializer.h"
#endif

#if !defined(OS_NACL)
#endif

namespace base {

namespace {

struct SequencedTask : public TrackingInfo  {
  SequencedTask()
      : sequence_token_id(0),
        trace_id(0),
        sequence_task_number(0),
        shutdown_behavior(SequencedWorkerPool::BLOCK_SHUTDOWN) {}

  explicit SequencedTask(const tracked_objects::Location& from_here)
      : base::TrackingInfo(from_here, TimeTicks()),
        sequence_token_id(0),
        trace_id(0),
        sequence_task_number(0),
        shutdown_behavior(SequencedWorkerPool::BLOCK_SHUTDOWN) {}

  ~SequencedTask() {}

  int sequence_token_id;
  int trace_id;
  int64 sequence_task_number;
  SequencedWorkerPool::WorkerShutdown shutdown_behavior;
  tracked_objects::Location posted_from;
  Closure task;

  // Non-delayed tasks and delayed tasks are managed together by time-to-run
  // order. We calculate the time by adding the posted time and the given delay.
  TimeTicks time_to_run;
};

struct SequencedTaskLessThan {
 public:
  bool operator()(const SequencedTask& lhs, const SequencedTask& rhs) const {
    if (lhs.time_to_run < rhs.time_to_run)
      return true;

    if (lhs.time_to_run > rhs.time_to_run)
      return false;

    // If the time happen to match, then we use the sequence number to decide.
    return lhs.sequence_task_number < rhs.sequence_task_number;
  }
};

base::LazyInstance<base::ThreadLocalPointer<
    SequencedWorkerPool::SequenceToken> >::Leaky g_lazy_tls_ptr =
        LAZY_INSTANCE_INITIALIZER;

}  // namespace

// Worker ---------------------------------------------------------------------

class SequencedWorkerPool::Worker : public SimpleThread {
 public:
  // Hold a (cyclic) ref to |worker_pool|, since we want to keep it
  // around as long as we are running.
  Worker(const scoped_refptr<SequencedWorkerPool>& worker_pool,
         int thread_number,
         const std::string& thread_name_prefix);
  ~Worker() override;

  // SimpleThread implementation. This actually runs the background thread.
  void Run() override;

  void set_running_task_info(SequenceToken token,
                             WorkerShutdown shutdown_behavior) {
    running_sequence_ = token;
    running_shutdown_behavior_ = shutdown_behavior;
  }

  SequenceToken running_sequence() const {
    return running_sequence_;
  }

  WorkerShutdown running_shutdown_behavior() const {
    return running_shutdown_behavior_;
  }

 private:
  scoped_refptr<SequencedWorkerPool> worker_pool_;
  SequenceToken running_sequence_;
  WorkerShutdown running_shutdown_behavior_;

  DISALLOW_COPY_AND_ASSIGN(Worker);
};

// Inner ----------------------------------------------------------------------

class SequencedWorkerPool::Inner {
 public:
  // Take a raw pointer to |worker| to avoid cycles (since we're owned
  // by it).
  Inner(SequencedWorkerPool* worker_pool, size_t max_threads,
        const std::string& thread_name_prefix,
        TestingObserver* observer);

  ~Inner();

  SequenceToken GetSequenceToken();

  SequenceToken GetNamedSequenceToken(const std::string& name);

  // This function accepts a name and an ID. If the name is null, the
  // token ID is used. This allows us to implement the optional name lookup
  // from a single function without having to enter the lock a separate time.
  bool PostTask(const std::string* optional_token_name,
                SequenceToken sequence_token,
                WorkerShutdown shutdown_behavior,
                const tracked_objects::Location& from_here,
                const Closure& task,
                TimeDelta delay);

  bool RunsTasksOnCurrentThread() const;

  bool IsRunningSequenceOnCurrentThread(SequenceToken sequence_token) const;

  void CleanupForTesting();

  void SignalHasWorkForTesting();

  int GetWorkSignalCountForTesting() const;

  void Shutdown(int max_blocking_tasks_after_shutdown);

  bool IsShutdownInProgress();

  // Runs the worker loop on the background thread.
  void ThreadLoop(Worker* this_worker);

 private:
  enum GetWorkStatus {
    GET_WORK_FOUND,
    GET_WORK_NOT_FOUND,
    GET_WORK_WAIT,
  };

  enum CleanupState {
    CLEANUP_REQUESTED,
    CLEANUP_STARTING,
    CLEANUP_RUNNING,
    CLEANUP_FINISHING,
    CLEANUP_DONE,
  };

  // Called from within the lock, this converts the given token name into a
  // token ID, creating a new one if necessary.
  int LockedGetNamedTokenID(const std::string& name);

  // Called from within the lock, this returns the next sequence task number.
  int64 LockedGetNextSequenceTaskNumber();

  // Called from within the lock, returns the shutdown behavior of the task
  // running on the currently executing worker thread. If invoked from a thread
  // that is not one of the workers, returns CONTINUE_ON_SHUTDOWN.
  WorkerShutdown LockedCurrentThreadShutdownBehavior() const;

  // Gets new task. There are 3 cases depending on the return value:
  //
  // 1) If the return value is |GET_WORK_FOUND|, |task| is filled in and should
  //    be run immediately.
  // 2) If the return value is |GET_WORK_NOT_FOUND|, there are no tasks to run,
  //    and |task| is not filled in. In this case, the caller should wait until
  //    a task is posted.
  // 3) If the return value is |GET_WORK_WAIT|, there are no tasks to run
  //    immediately, and |task| is not filled in. Likewise, |wait_time| is
  //    filled in the time to wait until the next task to run. In this case, the
  //    caller should wait the time.
  //
  // In any case, the calling code should clear the given
  // delete_these_outside_lock vector the next time the lock is released.
  // See the implementation for a more detailed description.
  GetWorkStatus GetWork(SequencedTask* task,
                        TimeDelta* wait_time,
                        std::vector<Closure>* delete_these_outside_lock);

  void HandleCleanup();

  // Peforms init and cleanup around running the given task. WillRun...
  // returns the value from PrepareToStartAdditionalThreadIfNecessary.
  // The calling code should call FinishStartingAdditionalThread once the
  // lock is released if the return values is nonzero.
  int WillRunWorkerTask(const SequencedTask& task);
  void DidRunWorkerTask(const SequencedTask& task);

  // Returns true if there are no threads currently running the given
  // sequence token.
  bool IsSequenceTokenRunnable(int sequence_token_id) const;

  // Checks if all threads are busy and the addition of one more could run an
  // additional task waiting in the queue. This must be called from within
  // the lock.
  //
  // If another thread is helpful, this will mark the thread as being in the
  // process of starting and returns the index of the new thread which will be
  // 0 or more. The caller should then call FinishStartingAdditionalThread to
  // complete initialization once the lock is released.
  //
  // If another thread is not necessary, returne 0;
  //
  // See the implementedion for more.
  int PrepareToStartAdditionalThreadIfHelpful();

  // The second part of thread creation after
  // PrepareToStartAdditionalThreadIfHelpful with the thread number it
  // generated. This actually creates the thread and should be called outside
  // the lock to avoid blocking important work starting a thread in the lock.
  void FinishStartingAdditionalThread(int thread_number);

  // Signal |has_work_| and increment |has_work_signal_count_|.
  void SignalHasWork();

  // Checks whether there is work left that's blocking shutdown. Must be
  // called inside the lock.
  bool CanShutdown() const;

  SequencedWorkerPool* const worker_pool_;

  // The last sequence number used. Managed by GetSequenceToken, since this
  // only does threadsafe increment operations, you do not need to hold the
  // lock. This is class-static to make SequenceTokens issued by
  // GetSequenceToken unique across SequencedWorkerPool instances.
  static base::StaticAtomicSequenceNumber g_last_sequence_number_;

  // This lock protects |everything in this class|. Do not read or modify
  // anything without holding this lock. Do not block while holding this
  // lock.
  mutable Lock lock_;

  // Condition variable that is waited on by worker threads until new
  // tasks are posted or shutdown starts.
  ConditionVariable has_work_cv_;

  // Condition variable that is waited on by non-worker threads (in
  // Shutdown()) until CanShutdown() goes to true.
  ConditionVariable can_shutdown_cv_;

  // The maximum number of worker threads we'll create.
  const size_t max_threads_;

  const std::string thread_name_prefix_;

  // Associates all known sequence token names with their IDs.
  std::map<std::string, int> named_sequence_tokens_;

  // Owning pointers to all threads we've created so far, indexed by
  // ID. Since we lazily create threads, this may be less than
  // max_threads_ and will be initially empty.
  typedef std::map<PlatformThreadId, linked_ptr<Worker> > ThreadMap;
  ThreadMap threads_;

  // Set to true when we're in the process of creating another thread.
  // See PrepareToStartAdditionalThreadIfHelpful for more.
  bool thread_being_created_;

  // Number of threads currently waiting for work.
  size_t waiting_thread_count_;

  // Number of threads currently running tasks that have the BLOCK_SHUTDOWN
  // or SKIP_ON_SHUTDOWN flag set.
  size_t blocking_shutdown_thread_count_;

  // A set of all pending tasks in time-to-run order. These are tasks that are
  // either waiting for a thread to run on, waiting for their time to run,
  // or blocked on a previous task in their sequence. We have to iterate over
  // the tasks by time-to-run order, so we use the set instead of the
  // traditional priority_queue.
  typedef std::set<SequencedTask, SequencedTaskLessThan> PendingTaskSet;
  PendingTaskSet pending_tasks_;

  // The next sequence number for a new sequenced task.
  int64 next_sequence_task_number_;

  // Number of tasks in the pending_tasks_ list that are marked as blocking
  // shutdown.
  size_t blocking_shutdown_pending_task_count_;

  // Lists all sequence tokens currently executing.
  std::set<int> current_sequences_;

  // An ID for each posted task to distinguish the task from others in traces.
  int trace_id_;

  // Set when Shutdown is called and no further tasks should be
  // allowed, though we may still be running existing tasks.
  bool shutdown_called_;

  // The number of new BLOCK_SHUTDOWN tasks that may be posted after Shudown()
  // has been called.
  int max_blocking_tasks_after_shutdown_;

  // State used to cleanup for testing, all guarded by lock_.
  CleanupState cleanup_state_;
  size_t cleanup_idlers_;
  ConditionVariable cleanup_cv_;

  TestingObserver* const testing_observer_;

  DISALLOW_COPY_AND_ASSIGN(Inner);
};

// Worker definitions ---------------------------------------------------------
SequencedWorkerPool::SequenceToken
SequencedWorkerPool::Inner::GetSequenceToken() {
  // Need to add one because StaticAtomicSequenceNumber starts at zero, which
  // is used as a sentinel value in SequenceTokens.
  return SequenceToken(g_last_sequence_number_.GetNext() + 1);
}

SequencedWorkerPool::SequenceToken
SequencedWorkerPool::Inner::GetNamedSequenceToken(const std::string& name) {
  AutoLock lock(lock_);
  return SequenceToken(LockedGetNamedTokenID(name));
}

bool SequencedWorkerPool::Inner::RunsTasksOnCurrentThread() const {
  AutoLock lock(lock_);
  return ContainsKey(threads_, PlatformThread::CurrentId());
}

bool SequencedWorkerPool::Inner::IsRunningSequenceOnCurrentThread(
    SequenceToken sequence_token) const {
  AutoLock lock(lock_);
  ThreadMap::const_iterator found = threads_.find(PlatformThread::CurrentId());
  if (found == threads_.end())
    return false;
  return sequence_token.Equals(found->second->running_sequence());
}

bool SequencedWorkerPool::Inner::IsShutdownInProgress() {
    AutoLock lock(lock_);
    return shutdown_called_;
}

int SequencedWorkerPool::Inner::LockedGetNamedTokenID(
    const std::string& name) {
  lock_.AssertAcquired();
  DCHECK(!name.empty());

  std::map<std::string, int>::const_iterator found =
      named_sequence_tokens_.find(name);
  if (found != named_sequence_tokens_.end())
    return found->second;  // Got an existing one.

  // Create a new one for this name.
  SequenceToken result = GetSequenceToken();
  named_sequence_tokens_.insert(std::make_pair(name, result.id_));
  return result.id_;
}

int64 SequencedWorkerPool::Inner::LockedGetNextSequenceTaskNumber() {
  lock_.AssertAcquired();
  // We assume that we never create enough tasks to wrap around.
  return next_sequence_task_number_++;
}

SequencedWorkerPool::WorkerShutdown
SequencedWorkerPool::Inner::LockedCurrentThreadShutdownBehavior() const {
  lock_.AssertAcquired();
  ThreadMap::const_iterator found = threads_.find(PlatformThread::CurrentId());
  if (found == threads_.end())
    return CONTINUE_ON_SHUTDOWN;
  return found->second->running_shutdown_behavior();
}

int SequencedWorkerPool::Inner::WillRunWorkerTask(const SequencedTask& task) {
  lock_.AssertAcquired();

  // Mark the task's sequence number as in use.
  if (task.sequence_token_id)
    current_sequences_.insert(task.sequence_token_id);

  // Ensure that threads running tasks posted with either SKIP_ON_SHUTDOWN
  // or BLOCK_SHUTDOWN will prevent shutdown until that task or thread
  // completes.
  if (task.shutdown_behavior != CONTINUE_ON_SHUTDOWN)
    blocking_shutdown_thread_count_++;

  // We just picked up a task. Since StartAdditionalThreadIfHelpful only
  // creates a new thread if there is no free one, there is a race when posting
  // tasks that many tasks could have been posted before a thread started
  // running them, so only one thread would have been created. So we also check
  // whether we should create more threads after removing our task from the
  // queue, which also has the nice side effect of creating the workers from
  // background threads rather than the main thread of the app.
  //
  // If another thread wasn't created, we want to wake up an existing thread
  // if there is one waiting to pick up the next task.
  //
  // Note that we really need to do this *before* running the task, not
  // after. Otherwise, if more than one task is posted, the creation of the
  // second thread (since we only create one at a time) will be blocked by
  // the execution of the first task, which could be arbitrarily long.
  return PrepareToStartAdditionalThreadIfHelpful();
}

void SequencedWorkerPool::Inner::DidRunWorkerTask(const SequencedTask& task) {
  lock_.AssertAcquired();

  if (task.shutdown_behavior != CONTINUE_ON_SHUTDOWN) {
    DCHECK_GT(blocking_shutdown_thread_count_, 0u);
    blocking_shutdown_thread_count_--;
  }

  if (task.sequence_token_id)
    current_sequences_.erase(task.sequence_token_id);
}

bool SequencedWorkerPool::Inner::IsSequenceTokenRunnable(
    int sequence_token_id) const {
  lock_.AssertAcquired();
  return !sequence_token_id ||
      current_sequences_.find(sequence_token_id) ==
          current_sequences_.end();
}

int SequencedWorkerPool::Inner::PrepareToStartAdditionalThreadIfHelpful() {
  lock_.AssertAcquired();
  // How thread creation works:
  //
  // We'de like to avoid creating threads with the lock held. However, we
  // need to be sure that we have an accurate accounting of the threads for
  // proper Joining and deltion on shutdown.
  //
  // We need to figure out if we need another thread with the lock held, which
  // is what this function does. It then marks us as in the process of creating
  // a thread. When we do shutdown, we wait until the thread_being_created_
  // flag is cleared, which ensures that the new thread is properly added to
  // all the data structures and we can't leak it. Once shutdown starts, we'll
  // refuse to create more threads or they would be leaked.
  //
  // Note that this creates a mostly benign race condition on shutdown that
  // will cause fewer workers to be created than one would expect. It isn't
  // much of an issue in real life, but affects some tests. Since we only spawn
  // one worker at a time, the following sequence of events can happen:
  //
  //  1. Main thread posts a bunch of unrelated tasks that would normally be
  //     run on separate threads.
  //  2. The first task post causes us to start a worker. Other tasks do not
  //     cause a worker to start since one is pending.
  //  3. Main thread initiates shutdown.
  //  4. No more threads are created since the shutdown_called_ flag is set.
  //
  // The result is that one may expect that max_threads_ workers to be created
  // given the workload, but in reality fewer may be created because the
  // sequence of thread creation on the background threads is racing with the
  // shutdown call.
  if (!shutdown_called_ &&
      !thread_being_created_ &&
      cleanup_state_ == CLEANUP_DONE &&
      threads_.size() < max_threads_ &&
      waiting_thread_count_ == 0) {
    // We could use an additional thread if there's work to be done.
    for (PendingTaskSet::const_iterator i = pending_tasks_.begin();
         i != pending_tasks_.end(); ++i) {
      if (IsSequenceTokenRunnable(i->sequence_token_id)) {
        // Found a runnable task, mark the thread as being started.
        thread_being_created_ = true;
        return static_cast<int>(threads_.size() + 1);
      }
    }
  }
  return 0;
}

bool SequencedWorkerPool::Inner::CanShutdown() const {
  lock_.AssertAcquired();
  // See PrepareToStartAdditionalThreadIfHelpful for how thread creation works.
  return !thread_being_created_ &&
         blocking_shutdown_thread_count_ == 0 &&
         blocking_shutdown_pending_task_count_ == 0;
}

base::StaticAtomicSequenceNumber
SequencedWorkerPool::Inner::g_last_sequence_number_;

// SequencedWorkerPool --------------------------------------------------------

// static
SequencedWorkerPool::SequenceToken
SequencedWorkerPool::GetSequenceTokenForCurrentThread() {
  // Don't construct lazy instance on check.
  if (g_lazy_tls_ptr == NULL)
    return SequenceToken();

  SequencedWorkerPool::SequenceToken* token = g_lazy_tls_ptr.Get().Get();
  if (!token)
    return SequenceToken();
  return *token;
}

SequencedWorkerPool::SequenceToken SequencedWorkerPool::GetSequenceToken() {
  return inner_->GetSequenceToken();
}

SequencedWorkerPool::SequenceToken SequencedWorkerPool::GetNamedSequenceToken(
    const std::string& name) {
  return inner_->GetNamedSequenceToken(name);
}

bool SequencedWorkerPool::RunsTasksOnCurrentThread() const {
  return inner_->RunsTasksOnCurrentThread();
}

bool SequencedWorkerPool::IsRunningSequenceOnCurrentThread(
    SequenceToken sequence_token) const {
  return inner_->IsRunningSequenceOnCurrentThread(sequence_token);
}

bool SequencedWorkerPool::IsShutdownInProgress() {
  return inner_->IsShutdownInProgress();
}

}  // namespace base
