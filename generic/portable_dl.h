

#ifdef _WIN32
#include <windows.h>
typedef HMODULE PDlHandle;
// Open a library from a Tcl path object
static PDlHandle PDl_OpenFromObj(Tcl_Interp *interp, Tcl_Obj *pathObj) {
    const void *native = Tcl_FSGetNativePath(pathObj);
    if (native == NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("could not get native path", -1));
        return NULL;
    }
    // Safer search semantics (Win7+)
    HMODULE h =
        LoadLibraryExW((LPCWSTR)native, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (h == NULL) {
        DWORD e = GetLastError();
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("LoadLibraryExW failed: %lu", (unsigned long)e));
    }
    return h;
}

static void *PDl_Sym(Tcl_Interp *interp, PDlHandle h, const char *name) {
    FARPROC p = GetProcAddress(h, name);
    if (p == NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("GetProcAddress failed for symbol '%s'", name));
        return NULL;
    }
    return (void *)p;
}

static void PDl_Close(PDlHandle h) {
    if (h != NULL) {
        FreeLibrary(h);
    }
}
#else
#include <dlfcn.h>
typedef void *PDlHandle;
static PDlHandle PDl_OpenFromObj(Tcl_Interp *interp, Tcl_Obj *pathObj) {
    const void *native = Tcl_FSGetNativePath(pathObj);
    if (native == NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("could not get native path", -1));
        return NULL;
    }
    /* cppcheck-suppress misra-c2012-11.5 */
    /* cppcheck-suppress misra-c2012-17.3 */
    void *h = dlopen((const char *)native, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        /* cppcheck-suppress misra-c2012-17.3 */
        const char *msg = dlerror();
        Tcl_SetObjResult(interp, Tcl_NewStringObj(msg ? msg : "dlopen failed", -1));
    }
    return h;
}

static void *PDl_Sym(Tcl_Interp *interp, PDlHandle h, const char *name) {
    /* cppcheck-suppress misra-c2012-17.3 */
    dlerror(); // clear
    /* cppcheck-suppress misra-c2012-17.3 */
    void *p = dlsym(h, name);
    /* cppcheck-suppress misra-c2012-17.3 */
    const char *e = dlerror();
    if (e != NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e, -1));
        return NULL;
    }
    return p;
}

static void PDl_Close(PDlHandle h) {
    if (h != NULL) {
        /* cppcheck-suppress misra-c2012-17.3 */
        dlclose(h);
    }
}
#endif
