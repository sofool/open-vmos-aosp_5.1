/*
 * Copyright (C) 2007-2014 The Android Open Source Project
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
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if (FAKE_LOG_DEVICE == 0)
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <time.h>
#include <unistd.h>

#ifdef __BIONIC__
#include <android/set_abort_message.h>
#endif

#include <log/logd.h>
#include <log/logger.h>
#include <log/log_read.h>
#include <private/android_filesystem_config.h>

#define LOG_BUF_SIZE 1024

#if FAKE_LOG_DEVICE
/* This will be defined when building for the host. */
#include "fake_log_device.h"
#endif

static int __write_to_log_init(log_id_t, struct iovec *vec, size_t nr);
static int (*write_to_log)(log_id_t, struct iovec *vec, size_t nr) = __write_to_log_init;
#ifdef HAVE_PTHREADS
static pthread_mutex_t log_init_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef __unused
#define __unused  __attribute__((__unused__))
#endif

#if FAKE_LOG_DEVICE
static int log_fds[(int)LOG_ID_MAX] = { -1, -1, -1, -1, -1 };
#else
static int logd_fd = -1;
#endif
#include <unistd.h>


#include <unistd.h>  
#include <sys/types.h>
#include <sys/syscall.h>

/*
 * This is used by the C++ code to decide if it should write logs through
 * the C code.  Basically, if /dev/socket/logd is available, we're running in
 * the simulator rather than a desktop tool and want to use the device.
 */
static enum {
    kLogUninitialized, kLogNotAvailable, kLogAvailable
} g_log_status = kLogUninitialized;
int __android_log_dev_available(void)
{
    if (g_log_status == kLogUninitialized) {
        if (access("/dev/socket/logdw", W_OK) == 0)
            g_log_status = kLogAvailable;
        else
            g_log_status = kLogNotAvailable;
    }

    return (g_log_status == kLogAvailable);
}

#if !FAKE_LOG_DEVICE
/* give up, resources too limited */
static int __write_to_log_null(log_id_t log_fd __unused, struct iovec *vec __unused,
                               size_t nr __unused)
{
    return -1;
}
#endif

/* log_init_lock assumed */
static int __write_to_log_initialize()
{
    int i, ret = 0;

#if FAKE_LOG_DEVICE
    for (i = 0; i < LOG_ID_MAX; i++) {
        char buf[sizeof("/dev/log_system")];
        snprintf(buf, sizeof(buf), "/dev/log_%s", android_log_id_to_name(i));
        log_fds[i] = fakeLogOpen(buf, O_WRONLY);
    }
#else
    if (logd_fd >= 0) {
        i = logd_fd;
        logd_fd = -1;
        close(i);
    }

    i = socket(PF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (i < 0) {
        ret = -errno;
        write_to_log = __write_to_log_null;
    } else if (fcntl(i, F_SETFL, O_NONBLOCK) < 0) {
        ret = -errno;
        close(i);
        i = -1;
        write_to_log = __write_to_log_null;
    } else {
        struct sockaddr_un un;
        memset(&un, 0, sizeof(struct sockaddr_un));
        un.sun_family = AF_UNIX;
        strcpy(un.sun_path, "/dev/socket/logdw");

        if (connect(i, (struct sockaddr *)&un, sizeof(struct sockaddr_un)) < 0) {
            ret = -errno;
            close(i);
            i = -1;
        }
    }
    logd_fd = i;
#endif

    return ret;
}

static int __write_to_log_kernel(log_id_t log_id, struct iovec *vec, size_t nr)
{
    ssize_t ret;
#if FAKE_LOG_DEVICE
    int log_fd;

    if (/*(int)log_id >= 0 &&*/ (int)log_id < (int)LOG_ID_MAX) {
        log_fd = log_fds[(int)log_id];
    } else {
        return -EBADF;
    }
    do {
        ret = fakeLogWritev(log_fd, vec, nr);
        if (ret < 0) {
            ret = -errno;
        }
    } while (ret == -EINTR);
#else
    static const unsigned header_length = 3;
    struct iovec newVec[nr + header_length];
    typeof_log_id_t log_id_buf;
    uint16_t tid;
    struct timespec ts;
    log_time realtime_ts;
    size_t i, payload_size;
    static uid_t last_uid = AID_ROOT; /* logd *always* starts up as AID_ROOT */

    if (last_uid == AID_ROOT) { /* have we called to get the UID yet? */
        last_uid = getuid();
    }
    if (last_uid == AID_LOGD) { /* logd, after initialization and priv drop */
        /*
         * ignore log messages we send to ourself (logd).
         * Such log messages are often generated by libraries we depend on
         * which use standard Android logging.
         */
        return 0;
    }

    if (logd_fd < 0) {
        return -EBADF;
    }

    /*
     *  struct {
     *      // what we provide
     *      typeof_log_id_t  log_id;
     *      u16              tid;
     *      log_time         realtime;
     *      // caller provides
     *      union {
     *          struct {
     *              char     prio;
     *              char     payload[];
     *          } string;
     *          struct {
     *              uint32_t tag
     *              char     payload[];
     *          } binary;
     *      };
     *  };
     */

    clock_gettime(CLOCK_REALTIME, &ts);
    realtime_ts.tv_sec = ts.tv_sec;
    realtime_ts.tv_nsec = ts.tv_nsec;

    log_id_buf = log_id;
    tid = gettid();

    newVec[0].iov_base   = (unsigned char *) &log_id_buf;
    newVec[0].iov_len    = sizeof_log_id_t;
    newVec[1].iov_base   = (unsigned char *) &tid;
    newVec[1].iov_len    = sizeof(tid);
    newVec[2].iov_base   = (unsigned char *) &realtime_ts;
    newVec[2].iov_len    = sizeof(log_time);

    for (payload_size = 0, i = header_length; i < nr + header_length; i++) {
        newVec[i].iov_base = vec[i - header_length].iov_base;
        payload_size += newVec[i].iov_len = vec[i - header_length].iov_len;

        if (payload_size > LOGGER_ENTRY_MAX_PAYLOAD) {
            newVec[i].iov_len -= payload_size - LOGGER_ENTRY_MAX_PAYLOAD;
            if (newVec[i].iov_len) {
                ++i;
            }
            break;
        }
    }

    /*
     * The write below could be lost, but will never block.
     *
     * ENOTCONN occurs if logd dies.
     * EAGAIN occurs if logd is overloaded.
     */
    ret = writev(logd_fd, newVec, i);
    if (ret < 0) {
        ret = -errno;
        if (ret == -ENOTCONN) {
#ifdef HAVE_PTHREADS
            pthread_mutex_lock(&log_init_lock);
#endif
            ret = __write_to_log_initialize();
#ifdef HAVE_PTHREADS
            pthread_mutex_unlock(&log_init_lock);
#endif

            if (ret < 0) {
                return ret;
            }

            ret = writev(logd_fd, newVec, nr + header_length);
            if (ret < 0) {
                ret = -errno;
            }
        }
    }

    if (ret > (ssize_t)(sizeof_log_id_t + sizeof(tid) + sizeof(log_time))) {
        ret -= sizeof_log_id_t + sizeof(tid) + sizeof(log_time);
    }
#endif

    return ret;
}

#if FAKE_LOG_DEVICE
static const char *LOG_NAME[LOG_ID_MAX] = {
    [LOG_ID_MAIN] = "main",
    [LOG_ID_RADIO] = "radio",
    [LOG_ID_EVENTS] = "events",
    [LOG_ID_SYSTEM] = "system",
    [LOG_ID_CRASH] = "crash"
};

const char *android_log_id_to_name(log_id_t log_id)
{
    if (log_id >= LOG_ID_MAX) {
        log_id = LOG_ID_MAIN;
    }
    return LOG_NAME[log_id];
}
#endif

static int __write_to_log_init(log_id_t log_id, struct iovec *vec, size_t nr)
{
#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&log_init_lock);
#endif

    if (write_to_log == __write_to_log_init) {
        int ret;

        ret = __write_to_log_initialize();
        if (ret < 0) {
#ifdef HAVE_PTHREADS
            pthread_mutex_unlock(&log_init_lock);
#endif
            return ret;
        }

        write_to_log = __write_to_log_kernel;
    }

#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&log_init_lock);
#endif

    return write_to_log(log_id, vec, nr);
}

///////////////////////
// add log copy form klog.c
#if 1
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cutils/klog.h>

static int klog_fd = -1;


void klog_init2(void)
{
#if 0// def SHENG_RM
    static const char *name = "/dev/__kmsg__";
    if (klog_fd >= 0) return; /* Already initialized */

    if (mknod(name, S_IFCHR | 0600, (1 << 8) | 11) == 0) {
        klog_fd = open(name, O_WRONLY);
        if (klog_fd < 0)
                return;
        fcntl(klog_fd, F_SETFD, FD_CLOEXEC);
        unlink(name);
    }
#else
    static const char *name = "/dev/socket/socket/log/__my_logcat__";
    if (klog_fd >= 0) return; /* Already initialized */

    klog_fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (klog_fd < 0)
        return;
    {
        char szBuff[512];
        sprintf(szBuff, "gid=%d uid=%d\n", getgid(), getuid());
        write(klog_fd, szBuff, strlen(szBuff));
    }
#endif
}

#define LOG_BUF_MAX 1024

void klog_vwrite2(const char *fmt, va_list ap)
{
    char buf[LOG_BUF_MAX];

    //if (level > klog_level) return;

    if (klog_fd < 0) klog_init2();
    if (klog_fd < 0) return;

    vsnprintf(buf, LOG_BUF_MAX, fmt, ap);
    buf[LOG_BUF_MAX - 1] = 0;

    write(klog_fd, buf, strlen(buf));
}

void klog_write2(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klog_vwrite2( fmt, ap);
    va_end(ap);
}
#endif
////////////////////
#include <sys/syscall.h>

#include <linux/unistd.h>

/*
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
*/

char *strLog[] = {
    "U",
    "D",
    "V",
    "D",
    "I",
    "W",
    "E",
    "F",
    "S",
};

// 类型
// 03-07 20:53:22.871  2034  2404 W GCoreFlp: No location to return for getLastLocation()
#include <stdio.h>
#include <time.h>
#define PRINT(fmt, ...) printf("%s   "fmt, get_cur_time(), ##__VA_ARGS__)
#define ERROR(fmt, ...) printf("%s   "fmt" :%s\n", get_cur_time(), ##__VA_ARGS__, strerror(errno))

// out 2019-03-08 12:09:34
static char *get_cur_time()
{
    static char s[2];
    time_t t;
    struct tm* ltime;

    time(&t);

    ltime = localtime(&t);

    strftime(s, 20, "%Y-%m-%d %H:%M:%S", ltime);

    return s;
}

int __android_log_write(int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    log_id_t log_id = LOG_ID_MAIN;
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS")) {
            log_id = LOG_ID_RADIO;
            /* Inform third party apps/ril/radio.. to use Rlog or RLOG */
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

#if __BIONIC__
    if (prio == ANDROID_LOG_FATAL) {
        android_set_abort_message(msg);
    }
#endif

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

#if 1
    return write_to_log(log_id, vec, 3);
#else

    if (access("/dev/socket/socket/logfile", F_OK) == 0)
    {
    #if FAKE_LOG_DEVICE
        klog_write2("<3>" "logcat[%d] %s: %s\n",getpid(), tag, msg);
    #else
        klog_write2("%s %d  %d %s %s: %s\n",get_cur_time(), getpid(), gettid(), strLog[prio], tag, msg);
    #endif
    }
    else
    {
        write_to_log(log_id, vec, 3);
    }

    return 0;
#endif

}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if ((bufID != LOG_ID_RADIO) &&
         (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS"))) {
            bufID = LOG_ID_RADIO;
            /* Inform third party apps/ril/radio.. to use Rlog or RLOG */
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    // if (access("/dev/socket/socket/logfile", F_OK) == 0)
    // {
    // #if FAKE_LOG_DEVICE
    //     klog_write2("<3>" "logcat[%d]" " %s: %s\n",getpid(), tag, msg);
    // #else
    //     klog_write2("%s %d  %d %s %s: %s\n",get_cur_time(), getpid(), gettid(), strLog[prio], tag, msg);
    // #endif
    // }
    // else
    // {
        write_to_log(bufID, vec, 3);
    // }
    return 0;   //write_to_log(bufID, vec, 3);
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[LOG_BUF_SIZE];

    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_buf_write(bufID, prio, tag, buf);
}

void __android_log_assert(const char *cond, const char *tag,
                          const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
        va_end(ap);
    } else {
        /* Msg not provided, log condition.  N.B. Do not use cond directly as
         * format string as it could contain spurious '%' syntax (e.g.
         * "%d" in "blocks%devs == 0").
         */
        if (cond)
            snprintf(buf, LOG_BUF_SIZE, "Assertion failed: %s", cond);
        else
            strcpy(buf, "Unspecified assertion failed");
    }

    __android_log_write(ANDROID_LOG_FATAL, tag, buf);
    __builtin_trap(); /* trap so we have a chance to debug the situation */
    /* NOTREACHED */
}

int __android_log_bwrite(int32_t tag, const void *payload, size_t len)
{
    struct iovec vec[2];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = (void*)payload;
    vec[1].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 2);
}

/*
 * Like __android_log_bwrite, but takes the type as well.  Doesn't work
 * for the general case where we're generating lists of stuff, but very
 * handy if we just want to dump an integer into the log.
 */
int __android_log_btwrite(int32_t tag, char type, const void *payload,
                          size_t len)
{
    struct iovec vec[3];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = &type;
    vec[1].iov_len = sizeof(type);
    vec[2].iov_base = (void*)payload;
    vec[2].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 3);
}

/*
 * Like __android_log_bwrite, but used for writing strings to the
 * event log.
 */
int __android_log_bswrite(int32_t tag, const char *payload)
{
    struct iovec vec[4];
    char type = EVENT_TYPE_STRING;
    uint32_t len = strlen(payload);

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = &type;
    vec[1].iov_len = sizeof(type);
    vec[2].iov_base = &len;
    vec[2].iov_len = sizeof(len);
    vec[3].iov_base = (void*)payload;
    vec[3].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 4);
}
