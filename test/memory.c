/*
 * memory.c -- Exercise the production SEND_DATA event without loading ngspice.
 *
 * Track temporary string/list objects created by bridge code. Before clearing
 * the result, each must either have been released or have a positive reference
 * count. This detects orphaned Tcl objects independently of allocator caching
 * and process RSS. The wrappers do not add references or change sharing.
 */
#undef USE_TCL_STUBS
#include <tcl.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    Tcl_Obj *obj;
    int released;
} TrackedObject;
static TrackedObject *tracked;
static size_t trackedCount, trackedCapacity;

static Tcl_Obj *Track(Tcl_Obj *obj) {
    if (trackedCount == trackedCapacity) {
        trackedCapacity = trackedCapacity ? trackedCapacity * 2 : 128;
        tracked = realloc(tracked, trackedCapacity * sizeof(*tracked));
        if (tracked == NULL) {
            abort();
        }
    }
    tracked[trackedCount++] = (TrackedObject){obj, 0};
    return obj;
}
static Tcl_Obj *NewString(const char *bytes, Tcl_Size length) { return Track(Tcl_NewStringObj(bytes, length)); }
static Tcl_Obj *NewList(Tcl_Size count, Tcl_Obj *const elements[]) { return Track(Tcl_NewListObj(count, elements)); }
static void Release(Tcl_Obj *obj) {
    if (obj->refCount <= 1) {
        for (size_t i = trackedCount; i > 0; i--) {
            if (tracked[i - 1].obj == obj && !tracked[i - 1].released) {
                tracked[i - 1].released = 1;
                break;
            }
        }
    }
    Tcl_DecrRefCount(obj);
}
#undef Tcl_NewStringObj
#undef Tcl_NewListObj
#undef Tcl_DecrRefCount
#define Tcl_NewStringObj NewString
#define Tcl_NewListObj NewList
#define Tcl_DecrRefCount Release
#include "../generic/ngspicetclbridge.c"
#undef Tcl_NewStringObj
#undef Tcl_NewListObj
#undef Tcl_DecrRefCount

static int Deliver(NgSpiceContext *ctx, int points) {
    DataBuf_Ensure(&ctx->prod, (size_t)points);
    ctx->prod.count = (size_t)points;
    for (int i = 0; i < points; i++) {
        DataRow *row = &ctx->prod.rows[i];
        row->veccount = 2;
        row->vecs = Tcl_Alloc(2 * sizeof(*row->vecs));
        row->vecs[0] = (DataCell){ckstrdup("real"), 0, (double)i, 0};
        row->vecs[1] = (DataCell){ckstrdup("complex"), 1, (double)i, -(double)i};
    }
    NgSpiceEvent event = {0};
    event.ctx = ctx;
    event.callbackId = SEND_DATA;
    event.gen = ctx->gen;
    Tcl_Preserve(ctx);
    NgSpiceEventProc((Tcl_Event *)&event, TCL_ALL_EVENTS);
    size_t orphans = 0;
    for (size_t i = 0; i < trackedCount; i++) {
        if (!tracked[i].released && tracked[i].obj->refCount == 0) {
            orphans++;
            /* Clean up failed-test temporaries too, without hiding the failure. */
            Tcl_DbDecrRefCount(tracked[i].obj, __FILE__, __LINE__);
        }
    }
    trackedCount = 0;
    if (orphans) {
        fprintf(stderr, "FAIL: %zu unowned string/list objects after SEND_DATA\n", orphans);
        return 0;
    }
    return 1;
}

static int CheckValues(Tcl_Interp *interp, Tcl_Obj *dict, int points) {
    const char *names[] = {"real", "complex"};
    for (int i = 0; i < 2; i++) {
        Tcl_Obj *key = Tcl_NewStringObj(names[i], -1), *values = NULL, *last = NULL;
        Tcl_Size length;
        double value;
        Tcl_DbIncrRefCount(key, __FILE__, __LINE__);
        int code = Tcl_DictObjGet(interp, dict, key, &values);
        Tcl_DbDecrRefCount(key, __FILE__, __LINE__);
        if (code != TCL_OK || values == NULL || Tcl_ListObjLength(interp, values, &length) != TCL_OK ||
            length != points * (i + 1)) {
            return 0;
        }
        Tcl_ListObjIndex(interp, values, length - 1, &last);
        if (Tcl_GetDoubleFromObj(interp, last, &value) != TCL_OK || value != (i ? -999.0 : 999.0)) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    Tcl_FindExecutable(argv[0]);
    Tcl_Interp *interp = Tcl_CreateInterp();
    NgSpiceContext ctx = {0};
    ctx.interp = interp;
    int ok = 1;
    for (int run = 0; run < 64 && ok; run++) {
        ctx.vectorData = Tcl_NewDictObj();
        Tcl_DbIncrRefCount(ctx.vectorData, __FILE__, __LINE__);
        ok = Deliver(&ctx, 1000) && CheckValues(interp, ctx.vectorData, 1000);
        Tcl_Obj *snapshot = ctx.vectorData;
        Tcl_DbIncrRefCount(snapshot, __FILE__, __LINE__);
        if (ok) {
            ok =
                Deliver(&ctx, 1000) && CheckValues(interp, snapshot, 1000) && CheckValues(interp, ctx.vectorData, 2000);
        }
        Tcl_DbDecrRefCount(snapshot, __FILE__, __LINE__);
        Tcl_DbDecrRefCount(ctx.vectorData, __FILE__, __LINE__);
        ctx.vectorData = NULL;
    }
    DataBuf_Free(&ctx.prod);
    Tcl_MutexFinalize(&ctx.mutex);
    free(tracked);
    Tcl_DeleteInterp(interp);
    Tcl_Finalize();
    if (!ok) {
        fprintf(stderr, "FAIL: memory ownership or vector/snapshot values\n");
        return 1;
    }
    puts("PASS: 64 repeated runs, real/complex data, shared snapshots, no orphaned temporaries");
    return 0;
}
