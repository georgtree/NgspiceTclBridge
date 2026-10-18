#include "ngspicetclbridge.h"

#ifdef HAVE_RBC
#include <rbcVector.h>
#include <rbcDecls.h>
#ifndef Rbc_VectorCheckWritable
#error "RBC 0.8.1 or newer stubs headers are required"
#endif
#include <rbcStubLib.c>
#endif

//***  BridgeRbcInit function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BridgeRbcInit --
 *
 *      Initializes the optional RBC vector stubs interface in the current interpreter.
 *      Requires RBC vector version 0.8.1 or a compatible later version.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter in which to initialize RBC vector support.
 *
 * Results:
 *      Returns TCL_OK if the RBC vector stubs interface is available.
 *      Returns TCL_ERROR if initialization fails or the extension was built without RBC support.
 *
 * Side Effects:
 *      May load the RBC vector package and evaluate Tcl code during package initialization.
 *      Initializes the RBC vector stubs pointer and sets the interpreter result on failure.
 *
 * Notes:
 *      Called only when vector output is required, before reading or publishing vector data.
 *      A previously initialized global stubs pointer does not establish that RBC is available
 *      in the current interpreter.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcInit(Tcl_Interp *interp) {
#ifdef HAVE_RBC
    return Rbc_VectorInitStubs(interp, "0.8.1", 0) ? TCL_OK : TCL_ERROR;
#else
    Tcl_SetObjResult(interp, Tcl_NewStringObj("ngspicetclbridge was built without RBC vector support", -1));
    return TCL_ERROR;
#endif
}

//***  BridgeRbcName function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BridgeRbcName --
 *
 *      Constructs a fully qualified RBC vector destination from a namespace and raw signal name.
 *      Preserves supported literal names and encodes other names to avoid invalid or ambiguous destinations.
 *
 * Parameters:
 *      const char *ns      - Canonical fully qualified destination namespace.
 *      const char *rawName - Null-terminated raw signal name.
 *
 * Results:
 *      Returns a new Tcl string object with reference count zero containing the qualified destination name.
 *
 * Side Effects:
 *      Allocates the result object. Does not create namespaces or vectors.
 *
 * Notes:
 *      Literal names may contain ASCII letters, digits, underscores, dots, at signs, balanced parentheses,
 *      and isolated interior colons. Empty names, names containing namespace separators, leading or trailing
 *      colons, unsupported characters, unbalanced parentheses, or the reserved "_raw_" prefix are encoded.
 *      Encoding uses "_raw_" followed by uppercase hexadecimal pairs for every byte of the raw name.
 *      Reserving the prefix prevents encoded names from colliding with preserved literal names.
 *      Does not check destination availability or interpret #auto as an automatic-naming request.
 *
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

//***  BindingTrace function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BindingTrace --
 *
 *      Tracks deletion or renaming of a live vector command.
 *
 * Parameters:
 *      ClientData data      - Binding associated with the traced command.
 *      Tcl_Interp *interp   - Interpreter invoking the trace; unused.
 *      const char *oldName  - Previous command name; unused.
 *      const char *newName  - New fully qualified name for a rename notification.
 *      int flags           - Command trace flags identifying deletion or renaming.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Deletion permanently invalidates the binding and clears its command token.
 *      Renaming replaces the retained name object while preserving vector ownership.
 *
 * Notes:
 *      RBC updates its registry after command traces return; identity and type checks are deferred to CheckBinding.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
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

//***  CheckDestination function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * CheckDestination --
 *
 *      Checks whether a destination is available for a new or replacement vector.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter containing the destination.
 *      Tcl_Obj *name      - Fully qualified destination name.
 *      int complex       - Nonzero for complex output; zero for real output.
 *      int replace       - Nonzero to allow replacement of a compatible vector.
 *      Rbc_Vector **vec  - Receives an existing compatible vector, or NULL for an unused destination.
 *
 * Results:
 *      Returns TCL_OK for an unused destination or a writable same-type vector eligible for replacement.
 *      Returns TCL_ERROR for a simulator-maintained vector, command collision, or incompatible destination.
 *
 * Side Effects:
 *      Updates *vec and sets the interpreter result on failure. Does not modify any vector.
 *
 * Notes:
 *      Requires initialized RBC stubs. The output pointer is only usable after a successful return.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
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
        if (Rbc_VectorCheckWritable(interp, *vec) != TCL_OK) {
            return TCL_ERROR;
        }
    } else if (Tcl_GetCommandInfo(interp, s, &info)) {
        Tcl_SetObjResult(interp, Tcl_ObjPrintf("command \"%s\" already exists and is not an RBC vector", s));
        return TCL_ERROR;
    }
    return TCL_OK;
}

//***  CheckBinding function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * CheckBinding --
 *
 *      Validates the command identity, vector identity, and numeric type of a retained live binding.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter containing the binding.
 *      Binding *b        - Binding to validate.
 *      Rbc_Vector **vec  - Receives the borrowed RBC vector pointer on success.
 *
 * Results:
 *      Returns TCL_OK for a valid binding or TCL_ERROR if its command, vector, or type no longer matches.
 *
 * Side Effects:
 *      Updates *vec and sets the interpreter result on failure. Does not modify the vector.
 *
 * Notes:
 *      Does not check writability. The output pointer is only usable after a successful return.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
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

//***  CheckWritableBinding function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * CheckWritableBinding --
 *
 *      Validates a retained live binding and checks permission to modify its vector.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter containing the binding.
 *      Binding *b        - Binding to validate.
 *      Rbc_Vector **vec  - Receives the borrowed RBC vector pointer on success.
 *
 * Results:
 *      Returns TCL_OK if the binding is valid and writable, or TCL_ERROR otherwise.
 *
 * Side Effects:
 *      Updates *vec and sets the interpreter result on failure. Does not modify the vector.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int CheckWritableBinding(Tcl_Interp *interp, Binding *b, Rbc_Vector **vec) {
    if (CheckBinding(interp, b, vec) != TCL_OK) {
        return TCL_ERROR;
    }
    return Rbc_VectorCheckWritable(interp, *vec);
}

//***  CreateVector function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * CreateVector --
 *
 *      Creates a literal-name RBC vector without a mapped Tcl array variable.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context, preserved by the caller.
 *      Tcl_Obj **namePtr   - Caller-owned name reference, replaced with the actual RBC name after creation.
 *      int complex        - Nonzero for a complex vector; zero for a real vector.
 *      Rbc_Vector **vec   - Receives the borrowed vector pointer on success.
 *      Tcl_Command *token - Receives the vector command token on success.
 *
 * Results:
 *      Returns TCL_OK on success or the Tcl/RBC completion code on failure.
 *
 * Side Effects:
 *      Evaluates rbc::vector create and may invoke application callbacks.
 *      Releases the old name reference and retains the actual name, including any #auto expansion.
 *      Deletes the created command if the simulator was destroyed or vector lookup fails after creation.
 *
 * Notes:
 *      The caller must hold a reference to *namePtr and release its current value after this call.
 *      The name reference may have changed even if a subsequent validation fails.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int CreateVector(NgSpiceContext *ctx, Tcl_Obj **namePtr, int complex, Rbc_Vector **vec, Tcl_Command *token) {
    Tcl_Obj *name = *namePtr;
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
    Tcl_Obj *actual = Tcl_GetObjResult(ctx->interp);
    Tcl_IncrRefCount(actual);
    Tcl_DecrRefCount(*namePtr);
    *namePtr = name = actual;
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
    code = Rbc_GetVector(ctx->interp, Tcl_GetString(name), vec);
    if (code != TCL_OK) {
        Tcl_InterpState state = Tcl_SaveInterpState(ctx->interp, code);
        Tcl_DeleteCommandFromToken(ctx->interp, *token);
        *token = NULL;
        Tcl_RestoreInterpState(ctx->interp, state);
    }
    return code;
}

//***  FreeBinding function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * FreeBinding --
 *
 *      Releases a retained live binding and deletes its vector command only when owned by the simulator.
 *
 * Parameters:
 *      Tcl_Interp *interp - Interpreter containing the binding.
 *      Binding *b        - Binding to release; must have a valid hash entry.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Removes the command trace, deletes an owned vector command, and removes the hash entry.
 *      Releases the retained name and frees the binding. Adopted vector commands remain alive.
 *
 * Notes:
 *      The caller is responsible for removing the binding from any linked list or active-vector array.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
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
 *
 * BridgeRbcPrepare --
 *
 *      Prepares live RBC vector bindings for a new plot using its initialization snapshot.
 *      Reuses retained bindings, creates or adopts destinations for new signals, and establishes the active
 *      vector order. Clears all retained vectors, including signals absent from the new plot.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context owning the live bindings.
 *      const InitSnap *snap - Plot initialization snapshot containing signal names, numeric types, and column order.
 *
 * Results:
 *      Returns TCL_OK if all bindings are prepared and cleared successfully.
 *      Returns TCL_ERROR on an invalid binding, incompatible destination, vector creation or clearing failure,
 *      or a build without RBC support.
 *
 * Side Effects:
 *      Allocates binding storage on first use.
 *      Creates or adopts vectors and installs command traces for newly registered bindings.
 *      Replaces the active binding array and clears retained vector contents.
 *      May invoke Tcl code and RBC client callbacks. Sets the interpreter result on failure.
 *
 * Notes:
 *      Called in the Tcl event thread with initialized RBC stubs and a preserved simulator context.
 *      Existing bindings are reused independently of the collision policy; new attachments obey that policy.
 *      Validates retained bindings and candidate destinations before creating vectors, then rechecks destinations
 *      during acquisition.
 *      Acquisition failure removes bindings added by this call, deleting owned vectors but preserving adopted ones.
 *      Failure during the final clearing step does not roll back the new active mapping or completed clears.
 *
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
    /* Prepare clears retained signals too, even when absent from this plot. */
    for (Binding *b = r->head; b != NULL; b = b->next) {
        Rbc_Vector *vec;
        if (CheckWritableBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    for (int i = 0; i < snap->veccount; i++) {
        Tcl_HashEntry *entry = Tcl_FindHashEntry(&r->bindings, snap->vecs[i].name);
        Rbc_Vector *vec;
        int code;
        if (entry != NULL) {
            Binding *b = Tcl_GetHashValue(entry);
            code = CheckWritableBinding(ctx->interp, b, &vec);
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
                code = CreateVector(ctx, &name, !snap->vecs[i].is_real, &vec, &token);
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
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BridgeRbcClear --
 *
 *      Empties all retained live RBC vectors without deleting their commands or detaching graph clients.
 *      Includes bindings for signals absent from the current plot.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context owning the live bindings.
 *
 * Results:
 *      Returns TCL_OK if all vectors are cleared or no binding storage has been initialized.
 *      Returns TCL_ERROR on an invalid or read-only binding, simulator destruction, or unavailable RBC support.
 *      Propagates an RBC reset failure.
 *
 * Side Effects:
 *      Resets each vector to empty storage and clears its recorded length, capacity, and data pointer.
 *      RBC manages release of the previous vector storage and may notify clients.
 *      Preserves binding names, ownership, command traces, and the active binding array.
 *
 * Notes:
 *      Requires initialized RBC stubs and a preserved simulator context in the Tcl event thread.
 *      Checks every binding before modifying vectors, then rechecks each binding before resetting it.
 *      Clearing is not atomic: vectors already cleared remain empty if a later reset or validation fails.
 *      Does not discard queued samples or clear ngspice's own data; those operations are handled separately.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcClear(NgSpiceContext *ctx) {
#ifdef HAVE_RBC
    if (ctx->rbc == NULL) {
        return TCL_OK;
    }
    for (Binding *b = ctx->rbc->head; b != NULL; b = b->next) {
        Rbc_Vector *vec;
        if (CheckWritableBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    for (Binding *b = ctx->rbc->head; b != NULL; b = b->next) {
        Rbc_Vector *vec;
        if (ctx->destroying || CheckWritableBinding(ctx->interp, b, &vec) != TCL_OK) {
            return TCL_ERROR;
        }
        int code = b->complex ? Rbc_ResetComplexVector(vec, NULL, 0, 0, TCL_STATIC)
                              : Rbc_ResetVector(vec, NULL, 0, 0, TCL_STATIC);
        if (code != TCL_OK) {
            return code;
        }
        b->length = b->capacity = 0;
        b->data = NULL;
    }
    return TCL_OK;
#else
    return BridgeRbcInit(ctx->interp);
#endif
}

//***  BridgeRbcAppend function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BridgeRbcAppend --
 *
 *      Appends a detached batch of simulation samples to the active live RBC vectors.
 *      Copies numeric values directly into vector storage without creating per-sample Tcl objects.
 *      Reuses available capacity and grows buffers geometrically when necessary.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context containing the prepared live bindings.
 *      const DataBuf *rows - Borrowed batch of samples in the current plot's vector order.
 *
 * Results:
 *      Returns TCL_OK after appending the batch, or immediately for an empty batch in an RBC-enabled build.
 *      Returns TCL_ERROR on missing metadata, inconsistent sample layout, invalid or read-only bindings,
 *      externally modified storage, size overflow, allocation failure, or simulator destruction.
 *      Propagates RBC reset failures and reports an error in builds without RBC support.
 *
 * Side Effects:
 *      Extends active vectors and updates each binding's recorded data pointer, length, and capacity.
 *      Transfers newly allocated buffers to RBC on successful publication; frees them if publication fails.
 *      Publishes each vector once per batch through the RBC reset API, which may notify clients.
 *      Does not modify or free the input batch.
 *
 * Notes:
 *      Requires initialized RBC stubs and a preserved simulator context in the Tcl event thread.
 *      Validates the complete batch layout and all destinations' writability before appending.
 *      Rechecks each binding and its storage before writing, and its identity after publication.
 *      Appending is not atomic across vectors: completed updates remain if a later vector fails.
 *
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
    /* A locked later output must not allow an earlier vector to be appended. */
    for (int i = 0; i < r->count; i++) {
        Rbc_Vector *vec;
        if (CheckWritableBinding(ctx->interp, r->active[i], &vec) != TCL_OK) {
            return TCL_ERROR;
        }
    }
    for (int i = 0; i < r->count; i++) {
        Binding *b = r->active[i];
        Rbc_Vector *vec;
        if (ctx->destroying || CheckWritableBinding(ctx->interp, b, &vec) != TCL_OK) {
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
 *
 * BridgeRbcResult --
 *
 *      Builds a dictionary mapping original raw signal names to their current fully qualified RBC vector commands.
 *      Validates each retained binding before including it in the result.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context containing the live bindings and receiving the result.
 *
 * Results:
 *      Returns TCL_OK with the dictionary as the interpreter result.
 *      Returns an empty dictionary if no bindings have been initialized in an RBC-enabled build.
 *      Returns TCL_ERROR if a binding is invalid or the extension was built without RBC support.
 *
 * Side Effects:
 *      Allocates the result dictionary and its keys.
 *      Sets the interpreter result to the completed dictionary or an error diagnostic.
 *      Releases the temporary dictionary on validation failure without returning a partial mapping.
 *
 * Notes:
 *      Requires initialized RBC stubs and execution in the Tcl event thread.
 *      Includes all retained bindings, including signals absent from the current plot.
 *      Preserves original signal keys while reporting destination names updated by rename traces.
 *      Does not copy sample data, modify vectors, or require writable destinations.
 *
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
 *
 * BridgeRbcDestroy --
 *
 *      Releases all live RBC binding state associated with a simulator context.
 *      Deletes vector commands owned by the handle while preserving adopted vectors and their contents.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context whose live bindings are being released.
 *
 * Results:
 *      None. Does nothing if no binding storage exists or the extension was built without RBC support.
 *
 * Side Effects:
 *      Sets ctx->rbc to NULL before releasing bindings.
 *      Removes command traces and deletes surviving vector commands owned by the handle.
 *      Releases retained name objects, binding records, the hash table, the active binding array,
 *      and the BridgeRbc structure.
 *      Deleting owned vector commands may notify RBC clients.
 *
 * Notes:
 *      Called during final context cleanup after outstanding preserves have been released.
 *      Numeric buffers remain RBC-owned: deleting owned vectors releases their storage through RBC,
 *      while adopted vectors retain their last contents.
 *      Does not delete the destination namespace or independent snapshot vectors.
 *
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
 *
 * BridgeRbcSnapshot --
 *
 *      Publishes copied simulation samples as an independent RBC vector snapshot.
 *      Creates a new destination or replaces an existing compatible vector in place.
 *      Converts complex samples from ngspice's representation to RBC's complex storage format.
 *
 * Parameters:
 *      NgSpiceContext *ctx - Simulator context, preserved by the caller.
 *      Tcl_Obj *name      - Fully qualified destination name; a final ::#auto requests automatic naming.
 *      int complex        - Nonzero for ngcomplex_t samples; zero for double samples.
 *      Tcl_Size count     - Validated, nonnegative number of samples.
 *      void *samples      - Tcl-allocated sample buffer; ownership transfers to this function on entry.
 *      int replace        - Nonzero to allow replacement of an existing writable, same-type vector.
 *
 * Results:
 *      Returns TCL_OK with the actual destination vector name as the interpreter result.
 *      Returns TCL_ERROR on initialization, allocation, destination, creation, or publication failure.
 *
 * Side Effects:
 *      Consumes samples on both success and failure; the caller must not access or free them afterward.
 *      Transfers published numeric storage to RBC and releases temporary storage on failure.
 *      May load RBC, evaluate Tcl code, create a vector command, or update an existing vector.
 *      Removes a newly created destination on publication failure if its command token still matches.
 *      Sets the interpreter result and may notify RBC clients.
 *
 * Notes:
 *      Called in the Tcl event thread after releasing ngspice's realloc lock.
 *      Rejects destinations maintained as live bindings by any simulator handle.
 *      Does not register the snapshot as a live binding: it belongs to the caller and survives handle destruction.
 *      Retains name temporarily without consuming the caller's reference.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int BridgeRbcSnapshot(NgSpiceContext *ctx, Tcl_Obj *name, int complex, Tcl_Size count, void *samples, int replace) {
#ifdef HAVE_RBC
    Rbc_Vector *vec = NULL;
    Tcl_Command token = NULL;
    void *data = samples;
    int created = 0, code = TCL_ERROR;
    Tcl_IncrRefCount(name);
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
    const char *text = Tcl_GetString(name);
    size_t length = strlen(text);
    int automatic = length >= 7 && strcmp(text + length - 7, "::#auto") == 0;
    if (!automatic && CheckDestination(ctx->interp, name, complex, replace, &vec) != TCL_OK) {
        goto done;
    }
    if (vec == NULL) {
        if (CreateVector(ctx, &name, complex, &vec, &token) != TCL_OK) {
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
    Tcl_DecrRefCount(name);
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
