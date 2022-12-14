/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <poll.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <selinux/selinux.h>

#include <private/android_filesystem_config.h>

#include "ueventd.h"
#include "log.h"
#include "util.h"
#include "devices.h"
#include "ueventd_parser.h"

static char hardware[32];
static unsigned revision = 0;

static void import_kernel_nv(char *name, int in_qemu)
{
    if (*name != '\0') {
        char *value = strchr(name, '=');
        if (value != NULL) {
            *value++ = 0;
            if (!strcmp(name,"androidboot.hardware"))
            {
                strlcpy(hardware, value, sizeof(hardware));
            }
        }
    }
}

int ueventd_main(int argc, char **argv)
{
    struct pollfd ufd;
    int nr;
    char tmp[32];

    /*
     * init sets the umask to 077 for forked processes. We need to
     * create files with exact permissions, without modification by
     * the umask.
     */
    umask(000);

    /* Prevent fire-and-forget children from becoming zombies.
     * If we should need to wait() for some children in the future
     * (as opposed to none right now), double-forking here instead
     * of ignoring SIGCHLD may be the better solution.
     */
    signal(SIGCHLD, SIG_IGN);

    // open_devnull_stdio();
    klog_init();
#if LOG_UEVENTS
    /* Ensure we're at a logging level that will show the events */
    if (klog_get_level() < KLOG_INFO_LEVEL) {
        klog_set_level(KLOG_INFO_LEVEL);
    }
#endif

    union selinux_callback cb;
    cb.func_log = log_callback;
    selinux_set_callback(SELINUX_CB_LOG, cb);

    INFO("starting ueventd\n");

#if 0   //## SHENG
    /* Respect hardware passed in through the kernel cmd line. Here we will look
     * for androidboot.hardware param in kernel cmdline, and save its value in
     * hardware[]. */
    import_kernel_cmdline(0, import_kernel_nv);

    get_hardware_name(hardware, &revision);
#else
    strcpy(hardware, "goldfish");
#endif
    ueventd_parse_config_file("/ueventd.rc");

    snprintf(tmp, sizeof(tmp), "/ueventd.%s.rc", hardware);
    ueventd_parse_config_file(tmp);

#if 0
    device_init();

    ufd.events = POLLIN;
    ufd.fd = get_device_fd();

    while(1) {
        ufd.revents = 0;
        nr = poll(&ufd, 1, -1);
        if (nr <= 0)
            continue;
        if (ufd.revents & POLLIN)
               handle_device_fd();
    }
#endif
    while(1)
    {
        sleep(11);
    }
}

static int get_android_id(const char *id)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(android_ids); i++)
        if (!strcmp(id, android_ids[i].name))
            return android_ids[i].aid;
    return -1;
}

void set_device_permission(int nargs, char **args)
{
    char *name;
    char *attr = 0;
    mode_t perm;
    uid_t uid;
    gid_t gid;
    int prefix = 0;
    int wildcard = 0;
    char *endptr;
    int ret;
    char *tmp = 0;

    if (nargs == 0)
        return;

    if (args[0][0] == '#')
        return;

    name = args[0];

    if (!strncmp(name,"/sys/", 5) && (nargs == 5)) {
        INFO("/sys/ rule %s %s\n",args[0],args[1]);
        attr = args[1];
        args++;
        nargs--;
    }

    if (nargs != 4) {
        ERROR("invalid line ueventd.rc line for '%s'\n", args[0]);
        return;
    }

    /* If path starts with mtd@ lookup the mount number. */
    if (!strncmp(name, "mtd@", 4)) {
        int n = mtd_name_to_number(name + 4);
        if (n >= 0)
            asprintf(&tmp, "/dev/mtd/mtd%d", n);
        name = tmp;
    } else {
        int len = strlen(name);
        char *wildcard_chr = strchr(name, '*');
        if ((name[len - 1] == '*') &&
            (wildcard_chr == (name + len - 1))) {
            prefix = 1;
            name[len - 1] = '\0';
        } else if (wildcard_chr) {
            wildcard = 1;
        }
    }

    perm = strtol(args[1], &endptr, 8);
    if (!endptr || *endptr != '\0') {
        ERROR("invalid mode '%s'\n", args[1]);
        free(tmp);
        return;
    }

    ret = get_android_id(args[2]);
    if (ret < 0) {
        ERROR("invalid uid '%s'\n", args[2]);
        free(tmp);
        return;
    }
    uid = ret;

    ret = get_android_id(args[3]);
    if (ret < 0) {
        ERROR("invalid gid '%s'\n", args[3]);
        free(tmp);
        return;
    }
    gid = ret;

    add_dev_perms(name, attr, perm, uid, gid, prefix, wildcard);
    free(tmp);
}
