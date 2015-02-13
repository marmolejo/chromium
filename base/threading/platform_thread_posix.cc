// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/platform_thread.h"

#include <errno.h>
#include <sched.h>

#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/safe_strerror_posix.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread_restrictions.h"

#if defined(OS_MACOSX)
#include <sys/resource.h>
#include <algorithm>
#endif

#if defined(OS_LINUX)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace base {

void InitThreading();
void InitOnThread();
void TerminateOnThread();
size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes);

namespace {

struct ThreadParams {
  ThreadParams()
      : delegate(NULL),
        joinable(false),
        priority(kThreadPriority_Normal),
        handle(NULL),
        handle_set(false, false) {
  }

  PlatformThread::Delegate* delegate;
  bool joinable;
  ThreadPriority priority;
  PlatformThreadHandle* handle;
  WaitableEvent handle_set;
};

}  // namespace

// static
PlatformThreadId PlatformThread::CurrentId() {
  // Pthreads doesn't have the concept of a thread ID, so we have to reach down
  // into the kernel.
#if defined(OS_MACOSX)
  return pthread_mach_thread_np(pthread_self());
#elif defined(OS_LINUX)
  return syscall(__NR_gettid);
#elif defined(OS_ANDROID)
  return gettid();
#elif defined(OS_SOLARIS) || defined(OS_QNX)
  return pthread_self();
#elif defined(OS_NACL) && defined(__GLIBC__)
  return pthread_self();
#elif defined(OS_NACL) && !defined(__GLIBC__)
  // Pointers are 32-bits in NaCl.
  return reinterpret_cast<int32>(pthread_self());
#elif defined(OS_POSIX)
  return reinterpret_cast<int64>(pthread_self());
#endif
}

// static
PlatformThreadRef PlatformThread::CurrentRef() {
  return PlatformThreadRef(pthread_self());
}

// static
PlatformThreadHandle PlatformThread::CurrentHandle() {
  return PlatformThreadHandle(pthread_self(), CurrentId());
}

// static
void PlatformThread::YieldCurrentThread() {
  sched_yield();
}

}  // namespace base
