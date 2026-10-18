#ifdef _WIN32
#include <windows.h>
typedef HMODULE PDlHandle;

//***  PDl_OpenFromObj function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * PDl_OpenFromObj --
 *
 *      Opens a shared library using the native filesystem path obtained from a Tcl object.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter receiving error diagnostics.
 *      Tcl_Obj *pathObj   - Tcl object containing the shared-library path.
 *
 * Results:
 *      Returns a library handle on success or NULL if native-path conversion or library loading fails.
 *      The caller owns the returned handle and releases it through PDl_Close when unloading is appropriate.
 *
 * Side Effects:
 *      Loads the library and its dependencies, potentially executing library initialization routines.
 *      Sets the interpreter result on failure.
 *
 * Notes:
 *      On Windows, uses LoadLibraryExW with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR and
 *      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS.
 *      On POSIX systems, uses dlopen with RTLD_NOW and RTLD_LOCAL.
 *      Borrows the native path from pathObj without taking ownership of the object or path storage.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline PDlHandle PDl_OpenFromObj(Tcl_Interp *interp, Tcl_Obj *pathObj) {
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

//***  PDl_Sym function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * PDl_Sym --
 *
 *      Resolves a named symbol from an open shared library using the platform's dynamic loader.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter receiving error diagnostics.
 *      PDlHandle h        - Valid handle to an open shared library.
 *      const char *name   - Null-terminated symbol name.
 *
 * Results:
 *      Returns the symbol address on success or NULL on lookup failure.
 *      The returned address is borrowed and must not be freed by the caller.
 *
 * Side Effects:
 *      Sets the interpreter result on lookup failure.
 *      On POSIX systems, clears the previous dynamic-loader error before lookup and consumes the new error afterward.
 *
 * Notes:
 *      Uses GetProcAddress on Windows and dlsym on POSIX systems.
 *      The library must remain loaded while the returned address is used.
 *      On POSIX systems, a symbol may legitimately have a NULL address; a loader error, rather than the address
 *      alone, determines whether this function sets an error diagnostic.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline void *PDl_Sym(Tcl_Interp *interp, PDlHandle h, const char *name) {
    FARPROC p = GetProcAddress(h, name);
    if (p == NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("GetProcAddress failed for symbol '%s'", name));
        return NULL;
    }
    return (void *)p;
}

//***  PDl_Close function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * PDl_Close --
 *
 *      Releases a shared-library handle through the platform's dynamic loader.
 *
 * Parameters:
 *      PDlHandle h - Library handle to release, or NULL.
 *
 * Results:
 *      None. Does nothing if h is NULL.
 *      The platform loader's return status is ignored.
 *
 * Side Effects:
 *      Decrements the library's loader reference count and may unload it and execute finalization routines.
 *      Does not clear the caller's handle variable or report unloading errors through Tcl.
 *
 * Notes:
 *      Uses FreeLibrary on Windows and dlclose on POSIX systems.
 *      The caller must ensure that no code, callbacks, or data from the library remain in use before unloading.
 *      Symbol addresses obtained through PDl_Sym must not be used after the library is unloaded.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline void PDl_Close(PDlHandle h) {
    if (h != NULL) {
        FreeLibrary(h);
    }
}
#else
#include <dlfcn.h>
typedef void *PDlHandle;

//***  PDl_OpenFromObj function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * PDl_OpenFromObj --
 *
 *      Opens a shared library using the native filesystem path obtained from a Tcl object.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter receiving error diagnostics.
 *      Tcl_Obj *pathObj   - Tcl object containing the shared-library path.
 *
 * Results:
 *      Returns a library handle on success or NULL if native-path conversion or library loading fails.
 *      The caller owns the returned handle and releases it through PDl_Close when unloading is appropriate.
 *
 * Side Effects:
 *      Loads the library and its dependencies, potentially executing library initialization routines.
 *      Sets the interpreter result on failure.
 *
 * Notes:
 *      On Windows, uses LoadLibraryExW with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR and
 *      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS.
 *      On POSIX systems, uses dlopen with RTLD_NOW and RTLD_LOCAL.
 *      Borrows the native path from pathObj without taking ownership of the object or path storage.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline PDlHandle PDl_OpenFromObj(Tcl_Interp *interp, Tcl_Obj *pathObj) {
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

//***  PDl_Sym function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * PDl_Sym --
 *
 *      Resolves a named symbol from an open shared library using the platform's dynamic loader.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter receiving error diagnostics.
 *      PDlHandle h        - Valid handle to an open shared library.
 *      const char *name   - Null-terminated symbol name.
 *
 * Results:
 *      Returns the symbol address on success or NULL on lookup failure.
 *      The returned address is borrowed and must not be freed by the caller.
 *
 * Side Effects:
 *      Sets the interpreter result on lookup failure.
 *      On POSIX systems, clears the previous dynamic-loader error before lookup and consumes the new error afterward.
 *
 * Notes:
 *      Uses GetProcAddress on Windows and dlsym on POSIX systems.
 *      The library must remain loaded while the returned address is used.
 *      On POSIX systems, a symbol may legitimately have a NULL address; a loader error, rather than the address
 *      alone, determines whether this function sets an error diagnostic.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline void *PDl_Sym(Tcl_Interp *interp, PDlHandle h, const char *name) {
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

//***  PDl_Close function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * PDl_Close --
 *
 *      Releases a shared-library handle through the platform's dynamic loader.
 *
 * Parameters:
 *      PDlHandle h - Library handle to release, or NULL.
 *
 * Results:
 *      None. Does nothing if h is NULL.
 *      The platform loader's return status is ignored.
 *
 * Side Effects:
 *      Decrements the library's loader reference count and may unload it and execute finalization routines.
 *      Does not clear the caller's handle variable or report unloading errors through Tcl.
 *
 * Notes:
 *      Uses FreeLibrary on Windows and dlclose on POSIX systems.
 *      The caller must ensure that no code, callbacks, or data from the library remain in use before unloading.
 *      Symbol addresses obtained through PDl_Sym must not be used after the library is unloaded.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline void PDl_Close(PDlHandle h) {
    if (h != NULL) {
        /* cppcheck-suppress misra-c2012-17.3 */
        dlclose(h);
    }
}
#endif
