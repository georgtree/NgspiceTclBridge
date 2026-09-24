#include "ngspicetclbridge.h"

#ifdef HAVE_RBC
#include <rbcVector.h>
#include <rbcDecls.h>
#include <rbcStubLib.c>
#endif

//***  BridgeRbcInit function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcInit -- Load vector-only RBC stubs in this interpreter, only when vector output is requested.
 * Results: TCL_OK or a package/build diagnostic. Loading may execute Tcl; callers preserve their context.
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcInit(Tcl_Interp *interp) {
#ifdef HAVE_RBC
    return Rbc_VectorInitStubs(interp, "0.5.0", 0) ? TCL_OK : TCL_ERROR;
#else
    Tcl_SetObjResult(interp, Tcl_NewStringObj("ngspicetclbridge was built without RBC vector support", -1));
    return TCL_ERROR;
#endif
}

//***  BridgeRbcName function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcName -- Return an unreferenced name in a fixed namespace, using tclsimrawreader's naming convention.
 * Balanced parentheses and ordinary RBC characters remain literal. Unsafe names and the reserved _raw_ prefix
 * become _raw_ plus uppercase hexadecimal UTF-8 bytes, preventing collisions and raw namespace injection.
 *----------------------------------------------------------------------------------------------------------------------
 */
Tcl_Obj *BridgeRbcName(const char *ns, const char *rawName) {
    static const char hex[] = "0123456789ABCDEF";
    Tcl_Obj *name = Tcl_NewStringObj(ns, -1);
    Tcl_Size depth = 0;
    size_t length = strlen(rawName);
    int literal = (length > 0 && rawName[0] != ':' && rawName[length - 1] != ':' && strstr(rawName, "::") == NULL &&
                   strncmp(rawName, "_raw_", 5) != 0);
    for (const unsigned char *p = (const unsigned char *)rawName; literal && *p; p++) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')' && depth > 0) {
            depth--;
        } else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
                     *p == '.' || *p == ':' || *p == '@')) {
            literal = 0;
        }
    }
    if (strcmp(ns, "::") != 0) {
        Tcl_AppendToObj(name, "::", 2);
    }
    if (literal && depth == 0) {
        Tcl_AppendToObj(name, rawName, -1);
    } else {
        Tcl_AppendToObj(name, "_raw_", 5);
        for (const unsigned char *p = (const unsigned char *)rawName; *p; p++) {
            char bytes[2] = {hex[*p >> 4], hex[*p & 15]};
            Tcl_AppendToObj(name, bytes, 2);
        }
    }
    return name;
}

#ifdef HAVE_RBC
typedef struct Binding {
    Tcl_HashEntry *entry;
    Tcl_Obj *name;
    Tcl_Command token;
    int owned, invalid, complex;
    Tcl_Size length, capacity;
    void *data; /* Identity only: RBC owns the numeric storage. */
    struct Binding *next;
} Binding;

struct BridgeRbc {
    Tcl_HashTable bindings; /* Raw name -> persistent Binding. */
    Binding *head;
    Binding **active; /* Current plot's column order. */
    int count;
};

/* Follow the command token across renames. RBC updates its registry after
 * command traces return, so defer vector/type validation to CheckBinding.
 * Deletion remains permanent: never attach to a replacement with the same name. */
static void BindingTrace(ClientData data, Tcl_Interp *interp, const char *oldName, const char *newName, int flags) {
    Binding *b = data;
    (void)interp;
    (void)oldName;
    if (flags & TCL_TRACE_DELETE) {
        b->invalid = 1;
        b->token = NULL;
    } else if ((flags & TCL_TRACE_RENAME) && !b->invalid) {
        Tcl_Obj *name = Tcl_NewStringObj(newName, -1);
        Tcl_IncrRefCount(name);
        Tcl_DecrRefCount(b->name);
        b->name = name;
    }
}

static int CheckDestination(Tcl_Interp *interp, Tcl_Obj *name, int complex, int replace, Rbc_Vector **vec) {
    Tcl_CmdInfo info;
    const char *s = Tcl_GetString(name);
    *vec = NULL;
    if (Tcl_CommandTraceInfo(interp, s, TCL_TRACE_RENAME | TCL_TRACE_DELETE, BindingTrace, NULL) != NULL) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("vector \"%s\" is maintained by a simulator handle", s));
        return TCL_ERROR;
    }
    if (Rbc_VectorExists2(interp, s)) {
        if (!replace) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("vector \"%s\" already exists", s));
            return TCL_ERROR;
        }
        if (Rbc_GetVector(interp, s, vec) != TCL_OK) {
            return TCL_ERROR;
        }
        if (!Tcl_GetCommandInfo(interp, s, &info) || info.isNativeObjectProc != 2 ||
            info.objClientData2 != (void *)*vec) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("command \"%s\" is not the vector's instance command", s));
            return TCL_ERROR;
        }
        if (Rbc_VectorGetType(*vec) != (complex ? RBC_VECTOR_COMPLEX : RBC_VECTOR_REAL)) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("vector \"%s\" has a different numeric type", s));
            return TCL_ERROR;
        }
    } else if (Tcl_GetCommandInfo(interp, s, &info)) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("command \"%s\" already exists and is not an RBC vector", s));
        return TCL_ERROR;
    }
    return TCL_OK;
}

static int CheckBinding(Tcl_Interp *interp, Binding *b, Rbc_Vector **vec) {
    Tcl_CmdInfo info;
    if (b->invalid || b->token == NULL ||
        Tcl_FindCommand(interp, Tcl_GetString(b->name), NULL, TCL_GLOBAL_ONLY) != b->token) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("live vector \"%s\" was deleted or renamed", Tcl_GetString(b->name)));
        return TCL_ERROR;
    }
    if (Rbc_GetVector(interp, Tcl_GetString(b->name), vec) != TCL_OK) {
        return TCL_ERROR;
    }
    if (!Tcl_GetCommandInfoFromToken(b->token, &info) || info.isNativeObjectProc != 2 ||
        info.objClientData2 != (void *)*vec ||
        Rbc_VectorGetType(*vec) != (b->complex ? RBC_VECTOR_COMPLEX : RBC_VECTOR_REAL)) {
        Tcl_SetObjResult(interp,
                         Tcl_ObjPrintf("live vector \"%s\" has changed identity or type", Tcl_GetString(b->name)));
        return TCL_ERROR;
    }
    return TCL_OK;
}

/* All Tcl calls use object arguments. Numeric-looking parentheses must remain literal names. */
static int CreateVector(NgSpiceContext *ctx, Tcl_Obj *name, int complex, Rbc_Vector **vec, Tcl_Command *token) {
    Tcl_Obj *args[] = {Tcl_NewStringObj("::rbc::vector", -1),
                       Tcl_NewStringObj("create", -1),
                       name,
                       Tcl_NewStringObj("-literal", -1),
                       Tcl_NewBooleanObj(1),
                       Tcl_NewStringObj("-variable", -1),
                       Tcl_NewObj(),
                       Tcl_NewStringObj("-type", -1),
                       Tcl_NewStringObj(complex ? "complex" : "real", -1)};
    for (int i = 0; i < 9; i++) {
        Tcl_IncrRefCount(args[i]);
    }
    int code = Tcl_EvalObjv(ctx->interp, 9, args, TCL_EVAL_DIRECT);
    for (int i = 0; i < 9; i++) {
        Tcl_DecrRefCount(args[i]);
    }
    if (code != TCL_OK) {
        return code;
    }
    *token = Tcl_FindCommand(ctx->interp, Tcl_GetString(name), NULL, TCL_GLOBAL_ONLY);
    if (*token == NULL || !Rbc_VectorExists2(ctx->interp, Tcl_GetString(name))) {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("RBC vector was removed during creation", -1));
        return TCL_ERROR;
    }
    if (ctx->destroying) {
        Tcl_DeleteCommandFromToken(ctx->interp, *token);
        *token = NULL;
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("simulator handle was destroyed during vector creation", -1));
        return TCL_ERROR;
    }
    return Rbc_GetVector(ctx->interp, Tcl_GetString(name), vec);
}

static void FreeBinding(Tcl_Interp *interp, Binding *b) {
    if (b->token != NULL) {
        Tcl_Obj *current = Tcl_NewObj();
        Tcl_IncrRefCount(current);
        Tcl_GetCommandFullName(interp, b->token, current);
        Tcl_UntraceCommand(interp, Tcl_GetString(current), TCL_TRACE_RENAME | TCL_TRACE_DELETE, BindingTrace, b);
        Tcl_DecrRefCount(current);
        if (b->owned) {
            Tcl_DeleteCommandFromToken(interp, b->token);
        }
    }
    Tcl_DeleteHashEntry(b->entry);
    Tcl_DecrRefCount(b->name);
    Tcl_Free(b);
}
#endif

//***  BridgeRbcPrepare function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcPrepare -- Acquire this plot's destinations before appending data. Called only in the Tcl event thread.
 * Existing bindings are reused independently of -ifexists. First attachments obey the collision policy. Preflight
 * all names/types before creating anything; roll back new registrations if creation fails, then clear every retained
 * binding so absent signals also become empty. RBC notifications may invoke application code; this is not a
 * transaction.
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcPrepare(NgSpiceContext *ctx, const InitSnap *snap) {
#ifdef HAVE_RBC
    if (ctx->rbc == NULL) {
        ctx->rbc = Tcl_Alloc(sizeof(*ctx->rbc));
        memset(ctx->rbc, 0, sizeof(*ctx->rbc));
        Tcl_InitHashTable(&ctx->rbc->bindings, TCL_STRING_KEYS);
    }
    BridgeRbc *r = ctx->rbc;
    for (int i = 0; i < snap->veccount; i++) {
        Tcl_HashEntry *entry = Tcl_FindHashEntry(&r->bindings, snap->vecs[i].name);
        Rbc_Vector *vec;
        int code;
        if (entry != NULL) {
            Binding *b = Tcl_GetHashValue(entry);
            code = CheckBinding(ctx->interp, b, &vec);
            if (code == TCL_OK && b->complex != !snap->vecs[i].is_real) {
                Tcl_SetObjResult(ctx->interp, Tcl_ObjPrintf("live vector \"%s\" has a different numeric type",
                                                            Tcl_GetString(b->name)));
                code = TCL_ERROR;
            }
        } else {
            Tcl_Obj *name = BridgeRbcName(Tcl_GetString(ctx->vectorNamespace), snap->vecs[i].name);
            Tcl_IncrRefCount(name);
            code = CheckDestination(ctx->interp, name, !snap->vecs[i].is_real, ctx->replaceVectors, &vec);
            Tcl_DecrRefCount(name);
        }
        if (code != TCL_OK) {
            return TCL_ERROR;
        }
    }
    Binding *oldHead = r->head;
    Binding **active = Tcl_Alloc((size_t)snap->veccount * sizeof(*active));
    for (int i = 0; i < snap->veccount; i++) {
        Tcl_HashEntry *entry = Tcl_FindHashEntry(&r->bindings, snap->vecs[i].name);
        if (entry == NULL) {
            Rbc_Vector *vec;
            Tcl_Command token = NULL;
            Tcl_Obj *name = BridgeRbcName(Tcl_GetString(ctx->vectorNamespace), snap->vecs[i].name);
            Tcl_IncrRefCount(name);
            int owned = 0, fresh;
            int code = CheckDestination(ctx->interp, name, !snap->vecs[i].is_real, ctx->replaceVectors, &vec);
            if (code == TCL_OK && vec == NULL) {
                code = CreateVector(ctx, name, !snap->vecs[i].is_real, &vec, &token);
                owned = 1;
            } else if (code == TCL_OK) {
                token = Tcl_FindCommand(ctx->interp, Tcl_GetString(name), NULL, TCL_GLOBAL_ONLY);
            }
            if (code != TCL_OK) {
                Tcl_DecrRefCount(name);
                Tcl_InterpState state = Tcl_SaveInterpState(ctx->interp, code);
                while (r->head != oldHead) {
                    Binding *b = r->head;
                    r->head = b->next;
                    FreeBinding(ctx->interp, b);
                }
                Tcl_RestoreInterpState(ctx->interp, state);
                Tcl_Free(active);
                return TCL_ERROR;
            }
            Binding *b = Tcl_Alloc(sizeof(*b));
            memset(b, 0, sizeof(*b));
            b->name = name;
            b->token = token;
            b->owned = owned;
            b->complex = !snap->vecs[i].is_real;
            b->entry = Tcl_CreateHashEntry(&r->bindings, snap->vecs[i].name, &fresh);
            Tcl_SetHashValue(b->entry, b);
            b->next = r->head;
            r->head = b;
            Tcl_TraceCommand(ctx->interp, Tcl_GetString(name), TCL_TRACE_RENAME | TCL_TRACE_DELETE, BindingTrace, b);
            entry = b->entry;
        }
        active[i] = Tcl_GetHashValue(entry);
    }
    Tcl_Free(r->active);
    r->active = active;
    r->count = snap->veccount;
    return BridgeRbcClear(ctx);
#else
    (void)snap;
    return BridgeRbcInit(ctx->interp);
#endif
}

//***  BridgeRbcClear function
/* Clear data without replacing commands or detaching graph clients. RBC retains ownership of its storage. */
int BridgeRbcClear(NgSpiceContext *ctx) {
#ifdef HAVE_RBC
    if (ctx->rbc == NULL) {
        return TCL_OK;
    }
    for (Binding *b = ctx->rbc->head; b != NULL; b = b->next) {
        Rbc_Vector *vec;
        if (CheckBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    for (Binding *b = ctx->rbc->head; b != NULL; b = b->next) {
        Rbc_Vector *vec;
        if (ctx->destroying || CheckBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
        b->length = b->capacity = 0;
        b->data = NULL;
        int code = b->complex ? Rbc_ResetComplexVector(vec, NULL, 0, 0, TCL_STATIC)
                              : Rbc_ResetVector(vec, NULL, 0, 0, TCL_STATIC);
        if (code != TCL_OK) {
            return code;
        }
    }
    return TCL_OK;
#else
    return BridgeRbcInit(ctx->interp);
#endif
}

//***  BridgeRbcAppend function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcAppend -- Append a detached batch directly to RBC storage, without per-sample Tcl objects.
 * Validate the complete layout before modifying data. Geometric capacity growth avoids copying the full history
 * on every batch. Filled storage is published once per vector through ResetVector, giving one notification per batch.
 * Attached vector contents are bridge-managed: external resizing/replacement is diagnosed rather than using stale data.
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcAppend(NgSpiceContext *ctx, const DataBuf *rows) {
#ifdef HAVE_RBC
    if (rows->count == 0) {
        return TCL_OK;
    }
    BridgeRbc *r = ctx->rbc;
    if (r == NULL || rows->count > (size_t)TCL_SIZE_MAX) {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("missing vector metadata or oversized data batch", -1));
        return TCL_ERROR;
    }
    for (size_t j = 0; j < rows->count; j++) {
        const DataRow *row = &rows->rows[j];
        if (row->veccount != r->count) {
            goto layout;
        }
        for (int i = 0; i < r->count; i++) {
            Binding *b = r->active[i];
            if (strcmp(row->vecs[i].name, Tcl_GetHashKey(&r->bindings, b->entry)) != 0 ||
                !!row->vecs[i].is_complex != b->complex) {
                goto layout;
            }
        }
    }
    for (int i = 0; i < r->count; i++) {
        Binding *b = r->active[i];
        Rbc_Vector *vec;
        if (ctx->destroying || CheckBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
        void *old = b->complex ? (void *)Rbc_VectorComplexData(vec) : (void *)Rbc_VectorData(vec);
        if (old != b->data || Rbc_VectorLength(vec) != b->length || Rbc_VectorSize(vec) != b->capacity) {
            Tcl_SetObjResult(ctx->interp, Tcl_ObjPrintf("live vector \"%s\" storage was modified outside the simulator",
                                                        Tcl_GetString(b->name)));
            return TCL_ERROR;
        }
        if ((Tcl_Size)rows->count > TCL_SIZE_MAX - b->length) {
            goto overflow;
        }
        Tcl_Size length = b->length + (Tcl_Size)rows->count;
        Tcl_Size capacity = b->capacity;
        size_t width = b->complex ? sizeof(Rbc_Complex) : sizeof(double);
        if (capacity < length) {
            capacity = capacity > 0 && capacity <= TCL_SIZE_MAX / 2 ? capacity * 2 : length;
            if (capacity < length) {
                capacity = length;
            }
            if (capacity < 256) {
                capacity = 256;
            }
        }
        if ((size_t)capacity > SIZE_MAX / width) {
            goto overflow;
        }
        void *data = old;
        if (capacity != b->capacity) {
            data = Tcl_AttemptAlloc((size_t)capacity * width);
            if (data == NULL) {
                Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("cannot allocate live vector storage", -1));
                return TCL_ERROR;
            }
            if (b->length) {
                memcpy(data, old, (size_t)b->length * width);
            }
        }
        for (size_t j = 0; j < rows->count; j++) {
            const DataCell *cell = &rows->rows[j].vecs[i];
            if (b->complex) {
                ((Rbc_Complex *)data)[b->length + (Tcl_Size)j] = (Rbc_Complex){cell->creal, cell->cimag};
            } else {
                ((double *)data)[b->length + (Tcl_Size)j] = cell->creal;
            }
        }
        int code = b->complex ? Rbc_ResetComplexVector(vec, data, length, capacity, TCL_DYNAMIC)
                              : Rbc_ResetVector(vec, data, length, capacity, TCL_DYNAMIC);
        if (code != TCL_OK) {
            if (data != old) {
                Tcl_Free(data);
            }
            return code;
        }
        b->data = data;
        b->length = length;
        b->capacity = capacity;
        if (ctx->destroying || CheckBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    return TCL_OK;
layout:
    Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("simulation data does not match live vector metadata", -1));
    return TCL_ERROR;
overflow:
    Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("live vector size overflow", -1));
    return TCL_ERROR;
#else
    (void)rows;
    return BridgeRbcInit(ctx->interp);
#endif
}

//***  BridgeRbcResult function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcResult -- Validate retained bindings and return raw signal names mapped to fully qualified commands.
 * Results: TCL_OK with a dictionary, or TCL_ERROR if a live destination has changed identity.
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcResult(NgSpiceContext *ctx) {
#ifdef HAVE_RBC
    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_IncrRefCount(dict);
    if (ctx->rbc != NULL) {
        for (Binding *b = ctx->rbc->head; b != NULL; b = b->next) {
            Rbc_Vector *vec;
            if (CheckBinding(ctx->interp, b, &vec) != TCL_OK) {
                Tcl_DecrRefCount(dict);
                return TCL_ERROR;
            }
            Tcl_Obj *key = Tcl_NewStringObj(Tcl_GetHashKey(&ctx->rbc->bindings, b->entry), -1);
            Tcl_DictObjPut(ctx->interp, dict, key, b->name);
        }
    }
    Tcl_SetObjResult(ctx->interp, dict);
    Tcl_DecrRefCount(dict);
    return TCL_OK;
#else
    return BridgeRbcInit(ctx->interp);
#endif
}

//***  BridgeRbcDestroy function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcDestroy -- Remove traces and delete only commands created by this handle.
 * Adopted vectors remain alive with their last RBC-owned buffers. Called when the preserved context is finally freed.
 *----------------------------------------------------------------------------------------------------------------------
 */
void BridgeRbcDestroy(NgSpiceContext *ctx) {
#ifdef HAVE_RBC
    BridgeRbc *r = ctx->rbc;
    if (r == NULL) {
        return;
    }
    ctx->rbc = NULL;
    while (r->head != NULL) {
        Binding *b = r->head;
        r->head = b->next;
        FreeBinding(ctx->interp, b);
    }
    Tcl_DeleteHashTable(&r->bindings);
    Tcl_Free(r->active);
    Tcl_Free(r);
#else
    (void)ctx;
#endif
}

//***  BridgeRbcSnapshot function
/*
 *----------------------------------------------------------------------------------------------------------------------
 * BridgeRbcSnapshot -- Publish a caller-owned, independent snapshot after ngspice's realloc lock is released.
 * Consumes samples on success and failure. A live binding (from any handle) is never a snapshot destination.
 * Newly created commands are removed on failure; existing same-type vectors are updated in place.
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcSnapshot(NgSpiceContext *ctx, Tcl_Obj *name, int complex, Tcl_Size count, void *samples, int replace) {
#ifdef HAVE_RBC
    Rbc_Vector *vec = NULL;
    Tcl_Command token = NULL;
    void *data = samples;
    int created = 0, code = TCL_ERROR;
    if (BridgeRbcInit(ctx->interp) != TCL_OK) {
        goto done;
    }
    if (ctx->destroying) {
        Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("simulator handle was destroyed while loading RBC", -1));
        goto done;
    }
    if (complex && count > 0) {
        if ((size_t)count > SIZE_MAX / sizeof(Rbc_Complex)) {
            Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("complex snapshot size overflow", -1));
            goto done;
        }
        data = Tcl_AttemptAlloc((size_t)count * sizeof(Rbc_Complex));
        if (data == NULL) {
            data = samples;
            Tcl_SetObjResult(ctx->interp, Tcl_NewStringObj("cannot allocate complex snapshot", -1));
            goto done;
        }
        for (Tcl_Size i = 0; i < count; i++) {
            ((Rbc_Complex *)data)[i] =
                (Rbc_Complex){((ngcomplex_t *)samples)[i].cx_real, ((ngcomplex_t *)samples)[i].cx_imag};
        }
        Tcl_Free(samples);
    }
    if (CheckDestination(ctx->interp, name, complex, replace, &vec) != TCL_OK) {
        goto done;
    }
    if (vec == NULL) {
        if (CreateVector(ctx, name, complex, &vec, &token) != TCL_OK) {
            goto done;
        }
        created = 1;
    }
    code = complex ? Rbc_ResetComplexVector(vec, data, count, count, TCL_DYNAMIC)
                   : Rbc_ResetVector(vec, data, count, count, TCL_DYNAMIC);
    if (code == TCL_OK) {
        data = NULL;
        Tcl_SetObjResult(ctx->interp, name);
    }
done:
    if (code != TCL_OK && created &&
        Tcl_FindCommand(ctx->interp, Tcl_GetString(name), NULL, TCL_GLOBAL_ONLY) == token) {
        Tcl_InterpState state = Tcl_SaveInterpState(ctx->interp, code);
        Tcl_DeleteCommandFromToken(ctx->interp, token);
        Tcl_RestoreInterpState(ctx->interp, state);
    }
    Tcl_Free(data);
    return code;
#else
    (void)name;
    (void)complex;
    (void)count;
    (void)replace;
    Tcl_Free(samples);
    return BridgeRbcInit(ctx->interp);
#endif
}
