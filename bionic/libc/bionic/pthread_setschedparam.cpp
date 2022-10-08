/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <errno.h>

#include "private/ErrnoRestorer.h"
#include "pthread_accessor.h"
#include "private/bionic_futex.h"


#define LIBCVM LIBCVM_pthread_setschedparam
#if LIBCVM==1
#include <__a_bionic_hook.h>
extern "C" long syscall_inner(long number, ...);

static inline bool isValidAddress(const void* addr) {
    char valid = '\0';
    if (syscall_inner(__NR_mincore, (void*)((size_t)addr & ~0xFFF), 1, (unsigned char*)&valid) != 0	) {
        if (errno == ENOMEM) {
            return false;
        }
    }
    return true;
}
int pthread_setschedparam(pthread_t t, int policy, const sched_param* param) {
  if (!isValidAddress((const void*)t)) {
    return 0;
  }
	return pthread_setschedparam_hook(t, policy, param);
}
#else
int pthread_setschedparam(pthread_t t, int policy, const sched_param* param) {
  ErrnoRestorer errno_restorer;

  pthread_accessor thread(t);
  if (thread.get() == NULL) {
    return ESRCH;
  }

  int rc = sched_setscheduler(thread->tid, policy, param);
  if (rc == -1) {
    return errno;
  }
  return 0;
}
#endif //#if LIBCVM==1
