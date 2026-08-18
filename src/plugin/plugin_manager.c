#include "plugin/plugin_manager.h"

#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define DL_HANDLE HMODULE
#define DL_OPEN(name) LoadLibraryA(name)
#define DL_SYM(handle, sym) ((void *)GetProcAddress(handle, sym))
#define DL_CLOSE(handle) FreeLibrary(handle)
#else
#include <dlfcn.h>
#define DL_HANDLE void *
#define DL_OPEN(name) dlopen(name, RTLD_NOW | RTLD_LOCAL)
#define DL_SYM(handle, sym) dlsym(handle, sym)
#define DL_CLOSE(handle) dlclose(handle)
#endif

#define MAX_PLUGINS 16

struct wtsn_plugin_manager {
    wtsn_plugin *plugins[MAX_PLUGINS];
    size_t count;
};

wtsn_plugin_manager *wtsn_plugin_manager_create(void) {
    return calloc(1, sizeof(wtsn_plugin_manager));
}

void wtsn_plugin_manager_destroy(wtsn_plugin_manager *m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        wtsn_plugin *p = m->plugins[i];
        if (!p) continue;
        if (p->shutdown && p->shutdown(p) == WTSN_OK) {
        }
        if (p->handle) DL_CLOSE((DL_HANDLE)p->handle);
        free(p);
    }
    free(m);
}

wtsn_error wtsn_plugin_manager_load(wtsn_plugin_manager *m, const char *path) {
    if (!m || !path || m->count >= MAX_PLUGINS) return WTSN_ERR_INVALID_ARG;

    DL_HANDLE h = DL_OPEN(path);
    if (!h) {
        wtsn_log(WTSN_LOG_WARN, "plugin load failed: %s", path);
        return WTSN_ERR_IO;
    }

    wtsn_plugin_create_fn create = (wtsn_plugin_create_fn)DL_SYM(h, "wtsn_plugin_create");
    if (!create) {
        DL_CLOSE(h);
        return WTSN_ERR_IO;
    }

    wtsn_plugin *p = create();
    if (!p) {
        DL_CLOSE(h);
        return WTSN_ERR_NO_MEMORY;
    }
    p->handle = (void *)h;
    m->plugins[m->count++] = p;
    wtsn_log(WTSN_LOG_INFO, "loaded plugin: %s", p->name);
    return WTSN_OK;
}

size_t wtsn_plugin_manager_count(wtsn_plugin_manager *m) {
    return m ? m->count : 0;
}

wtsn_plugin *wtsn_plugin_manager_get(wtsn_plugin_manager *m, size_t index) {
    if (!m || index >= m->count) return NULL;
    return m->plugins[index];
}
