

#ifdef _WIN32
#include <windows.h>
typedef HMODULE PDlHandle;
// Open a library from a Tcl path object
static PDlHandle PDl_OpenFromObj(Tcl_Interp *interp, Tcl_Obj *pathObj) {
    const void *native = Tcl_FSGetNativePath(pathObj);
    if (!native) {
        Tcl_SetResult(interp, "could not get native path", TCL_STATIC);
        return NULL;
    }
    // Safer search semantics (Win7+)
    HMODULE h =
        LoadLibraryExW((LPCWSTR)native, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) {
        DWORD e = GetLastError();
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("LoadLibraryExW failed: %lu", (unsigned long)e));
    }
    return h;
}
static void *PDl_Sym(Tcl_Interp *interp, PDlHandle h, const char *name) {
    FARPROC p = GetProcAddress(h, name);
    if (!p) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("GetProcAddress failed for symbol '%s'", name));
        return NULL;
    }
    return (void *)p;
}
static void PDl_Close(PDlHandle h) {
    if (h) {
        FreeLibrary(h);
    }
}
#else
#include <dlfcn.h>
typedef void *PDlHandle;
static PDlHandle PDl_OpenFromObj(Tcl_Interp *interp, Tcl_Obj *pathObj) {
    const void *native = Tcl_FSGetNativePath(pathObj);
    if (!native) {
        Tcl_SetResult(interp, "could not get native path", TCL_STATIC);
        return NULL;
    }
    void *h = dlopen((const char *)native, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *msg = dlerror();
        Tcl_SetObjResult(interp, Tcl_NewStringObj(msg ? msg : "dlopen failed", -1));
    }
    return h;
}
static void *PDl_Sym(Tcl_Interp *interp, PDlHandle h, const char *name) {
    dlerror(); // clear
    void *p = dlsym(h, name);
    const char *e = dlerror();
    if (e) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(e, -1));
        return NULL;
    }
    return p;
}
static void PDl_Close(PDlHandle h) {
    if (h) {
        dlclose(h);
    }
}
#endif
