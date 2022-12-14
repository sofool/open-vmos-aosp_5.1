/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <hardware/hardware.h>

#include <cutils/properties.h>

#include <dlfcn.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>

#define LOG_TAG "HAL"
#include <utils/Log.h>

/** Base path of the hal modules */
#if defined(__LP64__)
#define HAL_LIBRARY_PATH1 "/system/lib64/hw"
#define HAL_LIBRARY_PATH2 "/vendor/lib64/hw"
#else
#define HAL_LIBRARY_PATH1 "/system/lib/hw"
#define HAL_LIBRARY_PATH2 "/vendor/lib/hw"
#endif

#include <stdbool.h>
#include <__a_bionic_hook.h>

/**
 * There are a set of variant filename for modules. The form of the filename
 * is "<MODULE_ID>.variant.so" so for the led module the Dream variants 
 * of base "ro.product.board", "ro.board.platform" and "ro.arch" would be:
 *
 * led.trout.so
 * led.msm7k.so
 * led.ARMV6.so
 * led.default.so
 */

static const char *variant_keys[] = {
    "ro.hardware",  /* This goes first so that it can pick up a different
                       file on the emulator. */
    "ro.product.board",
    "ro.board.platform",
    "ro.arch"
};

static const int HAL_VARIANT_KEYS_COUNT =
    (sizeof(variant_keys)/sizeof(variant_keys[0]));

/**
 * Load the file defined by the variant and if successful
 * return the dlopen handle and the hmi.
 * @return 0 = success, !0 = failure.
 */
static int load(const char *id,
        const char *path,
        const struct hw_module_t **pHmi, bool bhost)
{
    int status;
    void *handle;
    struct hw_module_t *hmi;

    /*
     * load the symbols resolving undefined symbols before
     * dlopen returns. Since RTLD_GLOBAL is not or'd in with
     * RTLD_NOW the external symbols will not be global
     */
    if (bhost) {
        char prop[PATH_MAX];
        property_get("rf.force.load", prop, "0");
        int grallocversion = atoi(prop);
        if (grallocversion == 2) {
            char conver_buf[128]={0};
            int  conver_out_len = 0;		
            #if defined(__LP64__)
            strcpy(prop, get_abs_path((char*)"/system/lib64/libhwgralloc.so",128,conver_buf,&conver_out_len));
            #else
            strcpy(prop, get_abs_path((char*)"/system/lib/libhwgralloc.so",128,conver_buf,&conver_out_len));
            #endif
            path = prop;
            ALOGE("foce load=%s\n", path);
        }
        handle = vmosdlopen(path, RTLD_NOW);
    
    } else {
        handle = dlopen(path, RTLD_NOW);

//        if (strstr(path, "gralloc"))
//            while(true);
    }

    if (handle == NULL) {
        char const *err_str;
        if (bhost) {
            err_str = vmosdlerror();
        } else {
            err_str = dlerror();
        }

        ALOGE("load: module=%s\n%s", path, err_str?err_str:"unknown");
        status = -EINVAL;
        goto done;
    }

    /* Get the address of the struct hal_module_info. */
    const char *sym = HAL_MODULE_INFO_SYM_AS_STR;

    if (bhost) {
        hmi = (struct hw_module_t *)vmosdlsym(handle, sym);
    } else {
        hmi = (struct hw_module_t *)dlsym(handle, sym);
    }
    if (hmi == NULL) {
        ALOGE("load: couldn't find symbol %s", sym);
        status = -EINVAL;
        goto done;
    }

    /* Check that the id matches */
    if (strcmp(id, hmi->id) != 0) {
        ALOGE("load: id=%s != hmi->id=%s", id, hmi->id);
        status = -EINVAL;
        goto done;
    }

    hmi->dso = handle;

    /* success */
    status = 0;

    done:
    if (status != 0) {
        hmi = NULL;
        if (handle != NULL) {
            if (bhost) {
                vmosdlclose(handle);
            } else {
                dlclose(handle);
            }
            handle = NULL;
        }
    } else {
        ALOGV("loaded HAL id=%s path=%s hmi=%p handle=%p",
                id, path, *pHmi, handle);
    }

    *pHmi = hmi;

    return status;
}

/*
 * Check if a HAL with given name and subname exists, if so return 0, otherwise
 * otherwise return negative.  On success path will contain the path to the HAL.
 */
int host_faccessat(int dirfd, const char* pathname, int mode);
static int hw_module_exists(char *path, size_t path_len, const char *name,
                            const char *subname, bool bhost)
{
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif
    snprintf(path, path_len, "%s/%s.%s.so",
             HAL_LIBRARY_PATH2, name, subname);
    if (bhost) {
        if (host_faccessat(AT_FDCWD,path, R_OK) == 0)
            return 0;
    } else {
        if (access(path, R_OK) == 0)
            return 0;
    }

    snprintf(path, path_len, "%s/%s.%s.so",
             HAL_LIBRARY_PATH1, name, subname);

    if (bhost) {
        if (host_faccessat(AT_FDCWD,path, R_OK) == 0)
            return 0;
    } else {
        if (access(path, R_OK) == 0)
            return 0;
    }

    return -ENOENT;
}

int hw_get_module_by_class(const char *class_id, const char *inst,
                           const struct hw_module_t **module)
{
    int i;
    char prop[PATH_MAX];
    char path[PATH_MAX];
    char name[PATH_MAX];
    char prop_name[PATH_MAX];

    if (inst)
        snprintf(name, PATH_MAX, "%s.%s", class_id, inst);
    else
        strlcpy(name, class_id, PATH_MAX);

    /*
     * Here we rely on the fact that calling dlopen multiple times on
     * the same .so will simply increment a refcount (and not load
     * a new copy of the library).
     * We also assume that dlopen() is thread-safe.
     */
    bool bhost = false;
#if 0
    /* First try a property specific to the class and possibly instance */
    snprintf(prop_name, sizeof(prop_name), "ro.hardware.%s", name);
#else
    ALOGE("enter gralloc class_id=%s", class_id);

    int itest = 1;
    if (property_get("rf.test", prop, "1") > 0) {
        itest = atoi(prop);
    }
    if (itest&&strcmp(class_id, "gralloc") == 0) {
        snprintf(prop_name, sizeof(prop_name), "ro.host.hardware.%s", name);
        bhost = strcmp(class_id, "gralloc") == 0;
    } else {
        snprintf(prop_name, sizeof(prop_name), "ro.hardware.%s", name);
    }

#endif
    if (property_get(prop_name, prop, NULL) > 0) {
        if (hw_module_exists(path, sizeof(path), name, prop, bhost) == 0) {
            goto found;
        }
    }

    /* Loop through the configuration variants looking for a module */
    for (i=0 ; i<HAL_VARIANT_KEYS_COUNT; i++) {
        if (property_get(variant_keys[i], prop, NULL) == 0) {
            continue;
        }
        if (hw_module_exists(path, sizeof(path), name, prop, bhost) == 0) {
            goto found;
        }
    }

    /* Nothing found, try the default */
    if (hw_module_exists(path, sizeof(path), name, "default", bhost) == 0) {
        goto found;
    }

    return -ENOENT;

found:
    /* load the module, if this fails, we're doomed, and we should not try
     * to load a different variant. */
    return load(class_id, path, module, bhost);
}

int hw_get_module(const char *id, const struct hw_module_t **module)
{
    return hw_get_module_by_class(id, NULL, module);
}
