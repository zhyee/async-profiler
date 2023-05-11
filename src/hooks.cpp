/*
 * Copyright 2023 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include "hooks.h"
#include "os.h"
#include "perfEvents.h"
#include "profiler.h"


typedef void* (*ThreadFunc)(void*);

struct ThreadEntry {
    ThreadFunc start_routine;
    void* arg;
};

typedef int (*pthread_create_t)(pthread_t*, const pthread_attr_t*, ThreadFunc, void*);
static pthread_create_t _orig_pthread_create = NULL;

typedef void (*pthread_exit_t)(void*);
static pthread_exit_t _orig_pthread_exit = NULL;

static void* thread_start_wrapper(void* e) {
    ThreadEntry* entry = (ThreadEntry*)e;
    ThreadFunc start_routine = entry->start_routine;
    void* arg = entry->arg;
    free(entry);

    int threadId = OS::threadId();
    PerfEvents::createForThread(threadId);
    Log::debug("thread_start: %d", threadId);

    void* result = start_routine(arg);

    PerfEvents::destroyForThread(threadId);
    Log::debug("thread_end: %d", threadId);
    return result;
}

int pthread_create_hook(pthread_t* thread, const pthread_attr_t* attr, ThreadFunc start_routine, void* arg) {
    ThreadEntry* entry = (ThreadEntry*) malloc(sizeof(ThreadEntry));
    entry->start_routine = start_routine;
    entry->arg = arg;

    int result = _orig_pthread_create(thread, attr, thread_start_wrapper, entry);
    if (result != 0) {
        free(entry);
    }
   return result;
}

void pthread_exit_hook(void* retval) {
    Log::debug("thread_exit: %d", OS::threadId());
    _orig_pthread_exit(retval);
}


typedef void* (*dlopen_t)(const char*, int);
static dlopen_t _orig_dlopen = NULL;

void* dlopen_hook(const char* filename, int flags) {
    Log::debug("dlopen: %s", filename);
    void* result = _orig_dlopen(filename, flags);
    if (result != NULL) {
        Profiler::instance()->updateSymbols(false);
        Hooks::patchLibraries();
    }
    return result;
}


Mutex Hooks::_patch_lock;
int Hooks::_patched_libs = 0;

void Hooks::init() {
    _orig_pthread_create = pthread_create;
    _orig_pthread_exit = pthread_exit;
    _orig_dlopen = dlopen;
}

void Hooks::patchLibraries() {
    MutexLocker ml(_patch_lock);

    CodeCacheArray* native_libs = Profiler::instance()->nativeLibs();
    int native_lib_count = native_libs->count();

    while (_patched_libs < native_lib_count) {
        CodeCache* cc = (*native_libs)[_patched_libs++];
        cc->makeGotPatchable();

        for (void** entry = cc->gotStart(); entry < cc->gotEnd(); entry++) {
            const char* sym = cc->binarySearch(*entry);
            if (*entry == (void*)_orig_pthread_create || strcmp(sym, "pthread_create@plt") == 0) {
                *entry = (void*)pthread_create_hook;
            } else if (*entry == (void*)_orig_pthread_exit || strcmp(sym, "pthread_exit@plt") == 0) {
                *entry = (void*)pthread_exit_hook;
            } else if (*entry == (void*)_orig_dlopen || strcmp(sym, "dlopen@plt") == 0) {
                *entry = (void*)dlopen_hook;
            }
        }
    }
}
