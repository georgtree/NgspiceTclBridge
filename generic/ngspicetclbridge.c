#include "ngspicetclbridge.h"

/* global process-wide fuses */
static int g_disable_dlclose = 0;
static int g_heap_poisoned = 0;

/* void PrintRefCount(Tcl_Obj *objPtr) { */
/*     if (objPtr) { */
/*         fprintf(stderr, "refCount = %d\n", objPtr->refCount); */
/*         fflush(stderr); */
/*     } else { */
/*         fprintf(stderr, "objPtr is NULL\n"); */
/*         fflush(stderr); */
/*     } */
/* } */

//** small helpers
//***  ckstrdup function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * ckstrdup --
 *
 *      Allocate a copy of a NUL-terminated C string using Tcl's memory allocator.
 *
 * Parameters:
 *      const char *s               - input: pointer to a valid NUL-terminated string to duplicate
 *
 * Results:
 *      Returns a pointer to a newly allocated string buffer containing an exact copy of the input string,
 *      including the terminating NUL character. The returned pointer must be freed using Tcl_Free().
 *
 * Side Effects:
 *      Allocates memory from Tcl's memory subsystem via Tcl_Alloc().
 *      The allocation size is strlen(s) + 1 bytes.
 *      The contents are copied with memcpy(), so the source string must remain valid for the duration
 *      of the copy operation.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static char *ckstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)Tcl_Alloc(len);
    memcpy(copy, s, len);
    return copy;
}
//***  NameToEvtId function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * NameToEvtId --
 *
 *      Map a Tcl string object containing an event name to its corresponding internal event identifier constant.
 *
 * Parameters:
 *      Tcl_Obj *obj                - input: Tcl object whose string value specifies the event name
 *      int *idOut                  - output: pointer to an integer to receive the matched event ID
 *
 * Results:
 *      Returns TCL_OK if the event name matches one of the known entries in the internal name–ID mapping table,
 *      and stores the corresponding ID in *idOut.
 *      Returns TCL_ERROR if the string does not match any known event name.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int NameToEvtId(Tcl_Obj *obj, int *idOut) {
    const char *s = Tcl_GetString(obj);
    struct {
        const char *n;
        int id;
    } map[] = {
        {"send_char", SEND_CHAR}, {"send_stat", SEND_STAT},           {"controlled_exit", CONTROLLED_EXIT},
        {"send_data", SEND_DATA}, {"send_init_data", SEND_INIT_DATA}, {"bg_running", BG_THREAD_RUNNING},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(s, map[i].n) == 0) {
            *idOut = map[i].id;
            return TCL_OK;
        }
    }
    return TCL_ERROR;
}
//***  BumpAndSignal function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BumpAndSignal --
 *
 *      Atomically increment the specified event counter and signal any threads waiting on the
 *      associated condition variable.
 *
 * Parameters:
 *      NgSpiceContext *ctx          - input/output: pointer to the ngspice context containing the mutex,
 *                                      event counters, and condition variable
 *      int which                    - input: index into ctx->evt_counts[] identifying the event counter to increment
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Increments the selected event counter by 1 (monotonically).
 *      Acquires and releases ctx->mutex around the update and signal.
 *      Calls Tcl_ConditionNotify() to wake all threads waiting on ctx->cond.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline void BumpAndSignal(NgSpiceContext *ctx, int which) {
    Tcl_MutexLock(&ctx->mutex);
    ctx->evt_counts[which]++;
    Tcl_ConditionNotify(&ctx->cond);
    Tcl_MutexUnlock(&ctx->mutex);
}
//***  wait_for function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * wait_for --
 *
 *      Wait for a specific event counter in an NgSpiceContext to reach a target value, with optional timeout
 *      and abort handling. This function blocks until one of the following occurs:
 *
 *          - The specified event counter increases by at least `need` increments since the start of the wait
 *          - The context is marked for destruction (abort condition)
 *          - The timeout period expires
 *
 *      This allows waiting for multiple occurrences of a given event after the call to waitevent from Tcl.
 *      For example, with need = 3, the function will return only after three new events of the requested
 *      type have been registered since the call began.
 *
 * Parameters:
 *      NgSpiceContext *ctx   - input/output: pointer to the ngspice context containing the mutex,
 *                               event counters, and condition variable.
 *      int which             - input: index into ctx->evt_counts[] identifying the event to monitor.
 *      uint64_t need         - input: number of new events required before returning (minimum 1).
 *      long timeout_ms       - input: timeout in milliseconds; 0 or negative means wait indefinitely.
 *      int *reached_out      - output (optional): set to nonzero if the target count was reached
 *                               (i.e., the event fired `need` times) during the wait.
 *      uint64_t *count_out   - output (optional): set to the current cumulative total count for this
 *                               event when the wait terminates (since instance creation).
 *
 * Results:
 *      Returns one of:
 *          NGSPICE_WAIT_OK       - target count reached during the wait
 *          NGSPICE_WAIT_TIMEOUT  - timeout expired before target reached
 *          NGSPICE_WAIT_ABORTED  - context was marked for destruction before target reached
 *
 * Side Effects:
 *      Locks and unlocks ctx->mutex around counter checks and condition waits.
 *      May sleep in slices (25 ms by default) for finite timeouts, polling the counter between sleeps.
 *      For indefinite waits, uses Tcl_ConditionWait() to block until signaled or aborted.
 *
 * Notes:
 *      - The function checks event deltas relative to the counter value observed at entry time.
 *      - If multiple Tcl threads share a context, only one should call this function at a time.
 *      - The returned count is always cumulative; use `eventcounts -clear` in Tcl to zero counters
 *        if you prefer delta-based semantics between independent tests.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static wait_rc wait_for(NgSpiceContext *ctx, int which, uint64_t need, long timeout_ms, int *reached_out,
                        uint64_t *count_out) {
    if (need == 0) {
        need = 1;
    }
    const int slice_ms = 25;
    long remaining = timeout_ms;
    Tcl_MutexLock(&ctx->mutex);
    uint64_t start = ctx->evt_counts[which];
    uint64_t target = start + need;
    if (ctx->evt_counts[which] >= target) {
        uint64_t cnt = ctx->evt_counts[which];
        Tcl_MutexUnlock(&ctx->mutex);
        if (reached_out) {
            *reached_out = 1;
        }
        if (count_out) {
            *count_out = cnt;
        }
        return NGSPICE_WAIT_OK;
    }
    if (timeout_ms <= 0) {
        while (!ctx->destroying && !ctx->aborting && ctx->evt_counts[which] < target) {
            Tcl_ConditionWait(&ctx->cond, &ctx->mutex, NULL);
        }
    } else {
        while (!ctx->destroying && ctx->evt_counts[which] < target && remaining > 0) {
            Tcl_MutexUnlock(&ctx->mutex);
            int step = (remaining < slice_ms) ? (int)remaining : slice_ms;
            Tcl_Sleep(step);
            remaining -= step;
            Tcl_MutexLock(&ctx->mutex);
        }
    }
    int reached = (ctx->evt_counts[which] >= target);
    uint64_t cnt = ctx->evt_counts[which];
    int aborted = (ctx->destroying || ctx->aborting);
    Tcl_MutexUnlock(&ctx->mutex);
    if (reached_out) {
        *reached_out = reached;
    }
    if (count_out) {
        *count_out = cnt;
    }
    if (aborted) {
        return NGSPICE_WAIT_ABORTED;
    }
    if (!reached && timeout_ms > 0) {
        return NGSPICE_WAIT_TIMEOUT;
    }
    return NGSPICE_WAIT_OK;
}

//** DataBuf helpers
//***  DataBuf_Init function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * DataBuf_Init --
 *
 *      Initialize a DataBuf structure to an empty state.
 *
 * Parameters:
 *      DataBuf *b                   - output: pointer to the DataBuf structure to initialize
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Sets b->rows to NULL, b->count to 0, and b->cap to 0.
 *      Does not allocate any memory; the buffer remains empty until explicitly grown.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void DataBuf_Init(DataBuf *b) {
    b->rows = NULL;
    b->count = 0;
    b->cap = 0;
}
//***  FreeDataRow function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * FreeDataRow --
 *
 *      Release all memory associated with a single DataRow, including vector names and the vector array itself.
 *
 * Parameters:
 *      DataRow *r                   - input: pointer to the DataRow to free; may be NULL
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      If r is not NULL:
 *          - Iterates over r->vecs[0..veccount-1], freeing each vecs[i].name with Tcl_Free().
 *          - Frees the vecs array itself with Tcl_Free().
 *      The DataRow structure pointed to by r is not freed; caller retains ownership.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void FreeDataRow(DataRow *r) {
    if (!r) {
        return;
    }
    for (int i = 0; i < r->veccount; i++) {
        Tcl_Free(r->vecs[i].name);
    }
    Tcl_Free(r->vecs);
}
//***  DataBuf_Free function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * DataBuf_Free --
 *
 *      Release all memory associated with a DataBuf structure, including all contained DataRow entries.
 *
 * Parameters:
 *      DataBuf *b                     - input/output: pointer to the DataBuf structure to free
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Calls FreeDataRow() for each row in the buffer to release vector name strings and associated data.
 *      - Frees the underlying row array.
 *      - Resets the structure fields (rows pointer to NULL, count and capacity to 0).
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void DataBuf_Free(DataBuf *b) {
    for (size_t i = 0; i < b->count; i++) {
        FreeDataRow(&b->rows[i]);
    }
    Tcl_Free(b->rows);
    b->rows = NULL;
    b->count = 0;
    b->cap = 0;
}
//***  DataBuf_Ensure function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * DataBuf_Ensure --
 *
 *      Ensure that a DataBuf has capacity for at least a specified number of DataRow entries,
 *      expanding the buffer if necessary.
 *
 * Parameters:
 *      DataBuf *b                   - input/output: pointer to the DataBuf structure to check and resize
 *      size_t need                  - input: minimum number of DataRow slots required
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      If b->cap is less than 'need':
 *          - Computes a new capacity as either twice the current capacity, or 64 if the current capacity is zero.
 *          - If the computed capacity is still less than 'need', sets it to 'need'.
 *          - Resizes b->rows via Tcl_Realloc() to hold the new capacity in DataRow elements.
 *          - Updates b->cap to the new capacity.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void DataBuf_Ensure(DataBuf *b, size_t need) {
    if (need <= b->cap) {
        return;
    }
    size_t ncap = b->cap ? b->cap * 2 : 64;
    if (ncap < need) {
        ncap = need;
    }
    b->rows = (DataRow *)Tcl_Realloc(b->rows, ncap * sizeof(DataRow));
    b->cap = ncap;
}
//***  FreeInitSnap function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * FreeInitSnap --
 *
 *      Release all dynamically allocated memory associated with an InitSnap structure,
 *      including vector name strings, the vector array, and the structure itself.
 *
 * Parameters:
 *      InitSnap *snap               - input: pointer to the InitSnap structure to free; may be NULL
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      If snap is non-NULL:
 *          - Iterates over snap->vecs[0..veccount-1], freeing each vecs[i].name with Tcl_Free().
 *          - Frees the vecs array with Tcl_Free().
 *          - Frees the InitSnap structure itself with Tcl_Free().
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void FreeInitSnap(InitSnap *snap) {
    if (!snap) {
        return;
    }
    for (int i = 0; i < snap->veccount; i++) {
        Tcl_Free(snap->vecs[i].name);
    }
    Tcl_Free(snap->vecs);
    Tcl_Free(snap);
}

//***  DictLappend function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * DictLappend --
 *
 *      Append a list of values to an existing list stored at a given key in a Tcl dictionary.  
 *      If the key does not exist, a new list is created and inserted.
 *
 * Parameters:
 *      Tcl_Interp *interp         - input: target interpreter
 *      Tcl_Obj *dictObjPtr        - input: pointer to Tcl_Obj representing the dictionary (duplicated if shared)
 *      Tcl_Obj *keyObj            - input: pointer to Tcl_Obj representing the dictionary key to update
 *      Tcl_Obj *valuesList        - input: pointer to Tcl_Obj representing the list of values to append
 *
 * Results:
 *      Code TCL_OK - values were successfully appended or inserted
 *
 * Side Effects:
 *      If `dictObjPtr` or the existing list at `keyObj` is shared, it is duplicated before modification  
 *      A new list is created if the key does not exist in the dictionary  
 *      The updated list is stored under `keyObj` in the dictionary
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int DictLappend(Tcl_Interp *interp, Tcl_Obj *dictObjPtr, Tcl_Obj *keyObj, Tcl_Obj *valuesList) {
    Tcl_Obj *existingList = NULL;
    Tcl_Size listLen;
    Tcl_Obj **elements;
    if (Tcl_IsShared(dictObjPtr)) {
        dictObjPtr = Tcl_DuplicateObj(dictObjPtr);
    }
    Tcl_DictObjGet(interp, dictObjPtr, keyObj, &existingList);
    if (existingList == NULL) {
        existingList = Tcl_NewListObj(0, NULL);
    } else if (Tcl_IsShared(existingList)) {
        existingList = Tcl_DuplicateObj(existingList);
    }
    Tcl_ListObjGetElements(interp, valuesList, &listLen, &elements);
    for (Tcl_Size i = 0; i < listLen; i++) {
        Tcl_ListObjAppendElement(interp, existingList, elements[i]);
    }
    Tcl_DictObjPut(interp, dictObjPtr, keyObj, existingList);
    return TCL_OK;
}
//***  DictLappendElem function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * DictLappend --
 *
 *      Append a value to an existing list stored at a given key in a Tcl dictionary.  
 *      If the key does not exist, a new list is created and inserted.
 *
 * Parameters:
 *      Tcl_Interp *interp         - input: target interpreter
 *      Tcl_Obj *dictObjPtr        - input: pointer to Tcl_Obj representing the dictionary (duplicated if shared)
 *      Tcl_Obj *keyObj            - input: pointer to Tcl_Obj representing the dictionary key to update
 *      Tcl_Obj *valueObj          - input: pointer to Tcl_Obj representing the value to append
 *
 * Results:
 *      Code TCL_OK - values were successfully appended or inserted
 *
 * Side Effects:
 *      If `dictObjPtr` or the existing list at `keyObj` is shared, it is duplicated before modification  
 *      A new list is created if the key does not exist in the dictionary  
 *      The updated list is stored under `keyObj` in the dictionary
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int DictLappendElem(Tcl_Interp *interp, Tcl_Obj *dictObjPtr, Tcl_Obj *keyObj, Tcl_Obj *valueObj) {
    Tcl_Obj *existingList = NULL;
    if (Tcl_IsShared(dictObjPtr)) {
        dictObjPtr = Tcl_DuplicateObj(dictObjPtr);
    }
    Tcl_DictObjGet(interp, dictObjPtr, keyObj, &existingList);
    if (existingList == NULL) {
        existingList = Tcl_NewListObj(0, NULL);
    } else if (Tcl_IsShared(existingList)) {
        existingList = Tcl_DuplicateObj(existingList);
    }
    Tcl_ListObjAppendElement(interp, existingList, valueObj);
    Tcl_DictObjPut(interp, dictObjPtr, keyObj, existingList);
    return TCL_OK;
}

//** functions to work with message queue
//***  MsgQ_Init function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * MsgQ_Init --
 *
 *      Initialize a MsgQueue structure to an empty state.
 *
 * Parameters:
 *      MsgQueue *q                  - output: pointer to the MsgQueue structure to initialize
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Sets q->items to NULL, q->count to 0, and q->cap to 0.
 *      Does not allocate any memory; the queue remains empty until explicitly grown.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void MsgQ_Init(MsgQueue *q) {
    q->items = NULL;
    q->count = 0;
    q->cap = 0;
}
//***  MsgQ_Push function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * MsgQ_Push --
 *
 *      Append a copy of a message string to the end of a MsgQueue, expanding the queue's capacity if necessary.
 *
 * Parameters:
 *      MsgQueue *q                  - input/output: pointer to the message queue to modify
 *      const char *s                - input: NUL-terminated string to add to the queue
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      If q->count equals q->cap, grows the queue's storage:
 *          - New capacity is twice the current capacity, or 32 if the current capacity is zero.
 *          - Resizes q->items via Tcl_Realloc() to hold the new capacity.
 *      Allocates and stores a copy of the input string using ckstrdup().
 *      Increments q->count to reflect the added item.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void MsgQ_Push(MsgQueue *q, const char *s) {
    if (q->count == q->cap) {
        size_t ncap = q->cap ? q->cap * 2 : 32; // grow
        char **n = (char **)Tcl_Realloc(q->items, ncap * sizeof(char *));
        q->items = n;
        q->cap = ncap;
    }
    q->items[q->count++] = ckstrdup(s);
}
//***  MsgQ_Clear function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * MsgQ_Clear --
 *
 *      Remove all messages from a MsgQueue and free their associated memory,
 *      leaving the queue empty but retaining its allocated capacity.
 *
 * Parameters:
 *      MsgQueue *q                  - input/output: pointer to the message queue to clear
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Iterates over q->items[0..count-1], freeing each stored string with Tcl_Free().
 *      Sets q->count to 0.
 *      Does not free or shrink the q->items array; capacity remains available for reuse.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void MsgQ_Clear(MsgQueue *q) {
    for (size_t i = 0; i < q->count; i++) {
        Tcl_Free(q->items[i]);
    }
    q->count = 0;
}
//***  MsgQ_Free function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * MsgQ_Free --
 *
 *      Free all resources associated with a MsgQueue, including all stored messages
 *      and the message array itself, and reset the queue to an empty state.
 *
 * Parameters:
 *      MsgQueue *q                  - input/output: pointer to the message queue to free
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Calls MsgQ_Clear() to free all stored message strings.
 *      Frees the q->items array with Tcl_Free() and sets it to NULL.
 *      Sets q->cap to 0 and leaves q->count at 0.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void MsgQ_Free(MsgQueue *q) {
    MsgQ_Clear(q);
    Tcl_Free(q->items);
    q->items = NULL;
    q->cap = 0;
}
//***  QueueMsg function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * QueueMsg --
 *
 *      Thread-safe helper to append a message to the NgSpiceContext's message queue.
 *
 * Parameters:
 *      NgSpiceContext *ctx          - input/output: pointer to the ngspice context containing the target message queue
 *      const char *msg              - input: NUL-terminated string to enqueue
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Acquires ctx->mutex before modifying the queue to ensure thread safety.
 *      Calls MsgQ_Push() to add a copy of the message string to ctx->msgq.
 *      Releases ctx->mutex after the operation completes.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void QueueMsg(NgSpiceContext *ctx, const char *msg) {
    Tcl_MutexLock(&ctx->mutex);
    MsgQ_Push(&ctx->msgq, msg);
    Tcl_MutexUnlock(&ctx->mutex);
}

//** events processing
//***  NgSpiceEventProc function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * NgSpiceEventProc --
 *
 *      Tcl event handler for processing deferred ngspice callbacks in the main interpreter thread.
 *      This procedure is invoked via Tcl's event loop when ngspice-related events are queued from background
 *      threads or asynchronous callbacks. It unpacks the event data, updates shared state in NgSpiceContext,
 *      and performs any required Tcl_Obj manipulations in a thread-safe manner. If generation of the event is not the
 *      same as the current generation, skip event processing.
 *
 * Parameters:
 *      Tcl_Event *ev                - input: pointer to the queued Tcl event, castable to NgSpiceEvent
 *      int flags                    - input: bitmask of flags from Tcl's event loop (currently unused)
 *
 * Results:
 *      Always returns 1 to indicate the event was processed and should be removed from the queue.
 *
 * Side Effects:
 *      For SEND_INIT_DATA:
 *          - Retrieves and clears ctx->init_snap under ctx->mutex.
 *          - Resets previous generation vectorData
 *          - Builds a Tcl dictionary mapping vector names to metadata ("number" and "real").
 *          - Stores the dictionary in ctx->vectorInit, updating reference counts appropriately.
 *          - Frees the InitSnap structure.
 *
 *      For SEND_DATA:
 *          - Detaches ctx->prod into a local DataBuf under ctx->mutex, leaving ctx->prod empty.
 *          - Ensures ctx->vectorData is unshared if necessary by duplicating it.
 *          - Iterates over all DataRow entries, appending numeric or complex values into ctx->vectorData
 *            using DictLappend() or DictLappendElem().
 *          - Frees all DataRow entries and their vector name strings.
 *
 *      For all other callbacks:
 *          - No per-event processing is performed.
 *
 *      In all cases:
 *          - Calls Tcl_Release() on ctx to match a Tcl_Preserve() performed when queuing the event.
 *          - May allocate and free Tcl_Obj values.
 *          - May adjust Tcl reference counts and free dynamically allocated buffers.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int NgSpiceEventProc(Tcl_Event *ev, int flags) {
    NgSpiceEvent *sp = (NgSpiceEvent *)ev;
    NgSpiceContext *ctx = sp->ctx;
    Tcl_Interp *interp = ctx->interp;
    Tcl_MutexLock(&ctx->mutex);
    uint64_t curgen = ctx->gen;
    Tcl_MutexUnlock(&ctx->mutex);
    if (sp->gen != curgen) {
        Tcl_Release((ClientData)ctx);
        return 1;
    }
    switch ((enum CallbacksIds)sp->callbackId) {
    case SEND_INIT_DATA: {
        InitSnap *isnap = NULL;
        Tcl_MutexLock(&ctx->mutex);
        isnap = ctx->init_snap;
        ctx->init_snap = NULL;
        Tcl_MutexUnlock(&ctx->mutex);
        if (isnap) {
            Tcl_Obj *dict = Tcl_NewDictObj();
            for (int i = 0; i < isnap->veccount; i++) {
                Tcl_Obj *meta = Tcl_NewDictObj();
                Tcl_DictObjPut(interp, meta, Tcl_NewStringObj("number", -1), Tcl_NewIntObj(isnap->vecs[i].number));
                Tcl_DictObjPut(interp, meta, Tcl_NewStringObj("real", -1), Tcl_NewBooleanObj(isnap->vecs[i].is_real));
                Tcl_DictObjPut(interp, dict, Tcl_NewStringObj(isnap->vecs[i].name, -1), meta);
            }
            Tcl_MutexLock(&ctx->mutex);
            if (ctx->vectorInit) {
                Tcl_DecrRefCount(ctx->vectorInit);
            }
            ctx->vectorInit = dict;
            Tcl_IncrRefCount(dict);
            Tcl_MutexUnlock(&ctx->mutex);
            FreeInitSnap(isnap);
        }
        break;
    }
    case SEND_DATA: {
        DataBuf take;
        DataBuf_Init(&take);
        /* detach producer buffer */
        Tcl_MutexLock(&ctx->mutex);
        take = ctx->prod;
        ctx->prod.rows = NULL;
        ctx->prod.count = ctx->prod.cap = 0;
        if (Tcl_IsShared(ctx->vectorData)) {
            Tcl_Obj *dup = Tcl_DuplicateObj(ctx->vectorData);
            Tcl_IncrRefCount(dup);
            Tcl_DecrRefCount(ctx->vectorData);
            ctx->vectorData = dup;
        }
        Tcl_MutexUnlock(&ctx->mutex);
        for (size_t r = 0; r < take.count; r++) {
            DataRow *dr = &take.rows[r];
            for (int i = 0; i < dr->veccount; i++) {
                Tcl_Obj *key = Tcl_NewStringObj(dr->vecs[i].name, -1);
                if (dr->vecs[i].is_complex) {
                    Tcl_Obj *pair = Tcl_NewListObj(0, NULL);
                    Tcl_ListObjAppendElement(interp, pair, Tcl_NewDoubleObj(dr->vecs[i].creal));
                    Tcl_ListObjAppendElement(interp, pair, Tcl_NewDoubleObj(dr->vecs[i].cimag));
                    DictLappend(interp, ctx->vectorData, key, pair);
                } else {
                    DictLappendElem(interp, ctx->vectorData, key, Tcl_NewDoubleObj(dr->vecs[i].creal));
                }
            }
            FreeDataRow(dr);
        }
        Tcl_Free(take.rows);
        break;
    }
    default:
        break;
    }
    Tcl_Release((ClientData)ctx);
    return 1;
}
//***  DeleteNgSpiceEventProc function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * DeleteNgSpiceEventProc --
 *
 *      Predicate/deletion callback used with Tcl_DeleteEvents() to remove all pending ngspice-related
 *      events for a specific NgSpiceContext. This is called during teardown (InstDeleteProc) to ensure
 *      that no queued NgSpiceEvent objects referencing a soon-to-be-freed context are left in the Tcl
 *      event queue.
 *
 *      The procedure is invoked by Tcl for each queued event. If the event is an NgSpiceEvent belonging
 *      to the target context, this function:
 *
 *          - Releases the context reference previously preserved when the event was queued, and
 *          - Instructs Tcl to delete the event from the queue without delivering it.
 *
 *      Otherwise, it tells Tcl to leave the event alone.
 *
 * Parameters:
 *      Tcl_Event *evPtr          - input: pointer to a queued Tcl event. May or may not actually be
 *                                   an NgSpiceEvent. We assume NgSpiceEvent layout only after checking.
 *
 *      ClientData cd             - input: opaque pointer passed to Tcl_DeleteEvents(), expected to be
 *                                   the NgSpiceContext * whose events we want to purge.
 *
 * Results:
 *      Returns 1 if the event should be removed from the queue (and not processed later).
 *      Returns 0 if the event should be kept.
 *
 * Side Effects:
 *      - If evPtr is an NgSpiceEvent whose e->ctx matches the provided NgSpiceContext:
 *          - Calls Tcl_Release(e->ctx) to balance the Tcl_Preserve() done when the event
 *            was queued in NgSpiceQueueEvent().
 *          - Prevents NgSpiceEventProc from ever running for that event by returning 1.
 *
 *      - For all other events (not ours, or wrong instance):
 *          - Leaves them untouched by returning 0.
 *
 *      This function is critical for safe teardown:
 *          After InstDeleteProc() marks the context as destroying, it uses
 *          Tcl_DeleteEvents(DeleteNgSpiceEventProc, ctx) so that no stale NgSpiceEvent
 *          callbacks will later run and access freed memory.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int DeleteNgSpiceEventProc(Tcl_Event *evPtr, ClientData cd) {
    NgSpiceEvent *e = (NgSpiceEvent *)evPtr;
    NgSpiceContext *ctx = (NgSpiceContext *)cd;
    if (e->header.proc != NgSpiceEventProc) {
        return 0;
    }
    if (e->ctx != ctx) {
        return 0;
    }
    /* Balance the Tcl_Preserve(ctx) we did when queuing the event */
    Tcl_Release((ClientData)e->ctx);
    /* tell Tcl to delete (discard) this event */
    return 1;
}
//***  NgSpiceQueueEvent function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * NgSpiceQueueEvent --
 *
 *      Queue an NgSpiceEvent for processing in the Tcl event loop, preserving the NgSpiceContext
 *      until the event is handled. Events are delivered to the same thread that created the
 *      NgSpiceContext, regardless of the calling thread.
 *
 * Parameters:
 *      NgSpiceContext *ctx          - input/output: pointer to the ngspice context that owns the event
 *      int callbackId               - input: identifier for the type of callback/event to be processed
 *      uint64_t gen                 - input: generation identifier of the event
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - If ctx->destroying is nonzero, returns immediately without queuing an event.
 *      - Calls Tcl_Preserve(ctx) to keep the context alive until NgSpiceEventProc() calls Tcl_Release().
 *      - Allocates an NgSpiceEvent structure via Tcl_Alloc() and initializes its fields.
 *      - If called from the same thread as ctx->tclid, queues the event with Tcl_QueueEvent().
 *      - If called from a different thread, queues the event with Tcl_ThreadQueueEvent() and alerts
 *        the target thread with Tcl_ThreadAlert().
 *      - Event is processed by NgSpiceEventProc() when the Tcl event loop runs.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void NgSpiceQueueEvent(NgSpiceContext *ctx, int callbackId, uint64_t gen) {
    if (ctx->destroying) {
        return;
    }
    Tcl_Preserve((ClientData)ctx);
    NgSpiceEvent *ev = (NgSpiceEvent *)Tcl_Alloc(sizeof *ev);
    ev->header.proc = NgSpiceEventProc;
    ev->header.nextPtr = NULL;
    ev->ctx = ctx;
    ev->callbackId = callbackId;
    ev->gen = gen;
    if (Tcl_GetCurrentThread() == ctx->tclid) {
        Tcl_QueueEvent((Tcl_Event *)ev, TCL_QUEUE_TAIL);
    } else {
        Tcl_ThreadQueueEvent(ctx->tclid, (Tcl_Event *)ev, TCL_QUEUE_TAIL);
        Tcl_ThreadAlert(ctx->tclid);
    }
}
//***  QuiesceNgspice function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * QuiesceNgspice --
 *
 *      Requests ngspice to halt any ongoing background simulation activity and optionally waits for confirmation.
 *      Used during context destruction or controlled shutdown to ensure ngspice stops producing callbacks
 *      before issuing further cleanup commands.
 *
 * Parameters:
 *      NgSpiceContext *ctx            - input: pointer to the NgSpiceContext structure.
 *      int wait_ms                    - input: maximum number of milliseconds to wait for background thread to stop;
 *                                        if 0 or negative, returns immediately after sending the halt command.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Issues a "bg_halt" command through ctx->ngSpice_Command() to request ngspice to pause or stop background work.
 *      - If wait_ms > 0 and ctx->ngSpice_running is available:
 *          * Polls ngSpice_running() in small time slices (25 ms) up to wait_ms total.
 *          * Re-sends "bg_halt" every 200 ms if the background thread remains active.
 *      - Sleeps briefly between polling intervals to reduce CPU usage.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void QuiesceNgspice(NgSpiceContext *ctx, int wait_ms) {
    if (!ctx || !ctx->ngSpice_Command) {
        return;
    }
    if (ctx->ngSpice_running && ctx->ngSpice_running() == 1) {
        ctx->ngSpice_Command("bg_halt");
        if (wait_ms > 0) {
            const int slice = 25;
            int left = wait_ms;
            while (left > 0) {
                if (ctx->ngSpice_running() == 0)
                    break;
                if ((left % 200) == 0) {
                    ctx->ngSpice_Command("bg_halt");
                }
                Tcl_Sleep(slice);
                left -= slice;
            }
        }
    }
}
//***  EnqueuePending function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * EnqueuePending --
 *
 *      Adds a command to the pending command queue of an NgSpiceContext.
 *      Used when ngspice is in a transitional state (e.g., background thread
 *      starting or stopping) and cannot immediately process new commands.
 *
 * Parameters:
 *      NgSpiceContext *ctx    - input: target simulator context.
 *      const char *cmd        - input: command string to queue (copied).
 *      int capture            - input: 1 if this was invoked with -capture mode, else 0.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Allocates a new PendingCmd node containing a strdup of cmd.
 *      - Appends the node to ctx->pending_head/tail under ctx->cmd_mu.
 *      - The queued commands will later be executed by FlushPending().
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void EnqueuePending(NgSpiceContext *ctx, const char *cmd, int capture) {
    PendingCmd *n = (PendingCmd *)Tcl_Alloc(sizeof *n);
    n->cmd = ckstrdup(cmd);
    n->capture = capture;
    n->next = NULL;
    Tcl_MutexLock(&ctx->cmd_mu);
    if (ctx->pending_tail) {
        ctx->pending_tail->next = n;
    } else {
        ctx->pending_head = n;
    }
    ctx->pending_tail = n;
    Tcl_MutexUnlock(&ctx->cmd_mu);
}
//***  FlushPending function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * FlushPending --
 *
 *      Executes and clears all commands previously queued with EnqueuePending().
 *      This function is called when ngspice transitions back to a stable state
 *      (e.g., after the background thread starts or stops), allowing deferred
 *      commands to run safely.
 *
 * Parameters:
 *      NgSpiceContext *ctx    - input: simulator context whose pending queue will be flushed.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Takes ctx->cmd_mu to detach the entire queue atomically.
 *      - For each PendingCmd node:
 *            * If ctx->destroying is false, calls ctx->ngSpice_Command(cmd).
 *            * Frees the command string and node.
 *      - Leaves ctx->pending_head and ctx->pending_tail NULL after flushing.
 *      - If ctx->destroying is true or ngSpice_Command is unavailable, queued
 *        commands are silently discarded.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void FlushPending(NgSpiceContext *ctx) {
    Tcl_MutexLock(&ctx->cmd_mu);
    PendingCmd *list = ctx->pending_head;
    ctx->pending_head = NULL;
    ctx->pending_tail = NULL;
    Tcl_MutexUnlock(&ctx->cmd_mu);
    for (PendingCmd *p = list; p;) {
        PendingCmd *next = p->next;
        if (!ctx->destroying && ctx->ngSpice_Command) {
            ctx->ngSpice_Command((char *)p->cmd);
        }
        Tcl_Free(p->cmd);
        Tcl_Free(p);
        p = next;
    }
}
//***  MsgMaybeCaptureAndSignal function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * MsgMaybeCaptureAndSignal --
 *
 *      Pushes a message into the main ngspice message queue and, if capture mode is active,
 *      also duplicates it into the capture queue. Increments the event counter for the
 *      specified event type and signals any waiters.
 *
 * Parameters:
 *      NgSpiceContext *ctx            - input/output: pointer to the ngspice context holding
 *                                        message queues, mutex, condition variable, and event counters.
 *      const char *msg                - input: message string to enqueue (typically from a callback).
 *      int evt                        - input: index into ctx->evt_counts[] specifying the event type to update.
 *      uint64_t *gen_out              - output (optional): set to the current generation number (ctx->gen)
 *                                        at the time the message was queued.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Appends msg to ctx->msgq (always).
 *      - If ctx->cap_active is true, appends msg to ctx->capq as well.
 *      - Increments ctx->evt_counts[evt] and notifies ctx->cond to wake any waiters.
 *      - Returns the current generation counter through gen_out, if provided.
 *      - Thread-safe: protects all shared state with ctx->mutex.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static inline void MsgMaybeCaptureAndSignal(NgSpiceContext *ctx, const char *msg, int evt, uint64_t *gen_out) {
    Tcl_MutexLock(&ctx->mutex);
    MsgQ_Push(&ctx->msgq, msg);
    if (ctx->cap_active) {
        MsgQ_Push(&ctx->capq, msg);
    }
    ctx->evt_counts[evt]++;
    Tcl_ConditionNotify(&ctx->cond);
    if (gen_out) {
        *gen_out = ctx->gen;
    }
    Tcl_MutexUnlock(&ctx->mutex);
}
//** ngspice callbacks (instance-scoped via ctx user ptr)
//***  SendCharCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendCharCallback --
 *
 *      ngspice callback function invoked whenever new textual output is produced on ngspice’s stdout or stderr.
 *      Captures the message, updates event counters, signals waiting threads, and queues a Tcl event
 *      for deferred processing in the main thread.
 *
 * Parameters:
 *      char *msg                      - input: pointer to the NUL-terminated message string from ngspice.
 *      int id                         - input: identification number of the calling ngspice shared library instance
 *                                        (usually 0; can be ignored).
 *      void *user                     - input: user data pointer supplied during ngspice initialization,
 *                                        expected to be an NgSpiceContext *.
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid, msg is non-NULL, and ctx->destroying is false:
 *          - Appends msg to the main message queue (ctx->msgq), and if capture mode is active, also to ctx->capq.
 *          - Increments the SEND_CHAR event counter and signals any waiters on ctx->cond.
 *          - Queues a SEND_CHAR Tcl event (via NgSpiceQueueEvent) for main-thread processing.
 *      - Thread-safe: protects shared structures with ctx->mutex internally through MsgMaybeCaptureAndSignal().
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int SendCharCallback(char *msg, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx || !msg || ctx->destroying) {
        return 0;
    }
    uint64_t mygen = 0;
    MsgMaybeCaptureAndSignal(ctx, msg, SEND_CHAR, &mygen);
    NgSpiceQueueEvent(ctx, SEND_CHAR, mygen);
    return 0;
}
//***  SendStatCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendStatCallback --
 *
 *      ngspice callback function invoked whenever the simulator’s status changes
 *      (e.g., transitions such as "--ready--", "--stopped--", or "--running--").
 *      Formats the status message, records it in the message queue, updates the event
 *      counters, and queues a Tcl event for processing in the main thread.
 *
 * Parameters:
 *      char *msg                      - input: pointer to the NUL-terminated status message from ngspice.
 *      int id                         - input: identification number of the calling ngspice shared library instance.
 *      void *user                     - input: user data pointer supplied during ngspice initialization,
 *                                        expected to be an NgSpiceContext *.
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid and ctx->destroying is false:
 *          - Formats the message as: "# status[<id>]: <msg>".
 *          - Appends the formatted line to ctx->msgq via QueueMsg().
 *          - Increments the SEND_STAT event counter and signals waiters via BumpAndSignal().
 *          - Queues a SEND_STAT Tcl event via NgSpiceQueueEvent() for deferred processing.
 *      - Thread-safe: protects access to ctx->gen with ctx->mutex.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int SendStatCallback(char *msg, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    uint64_t mygen;
    if (!ctx || ctx->destroying) {
        return 0;
    }
    char line[128 + 1];
    snprintf(line, sizeof line, "# status[%d]: %s", id, msg);
    QueueMsg(ctx, line);
    BumpAndSignal(ctx, SEND_STAT);
    Tcl_MutexLock(&ctx->mutex);
    mygen = ctx->gen;
    Tcl_MutexUnlock(&ctx->mutex);
    NgSpiceQueueEvent(ctx, SEND_STAT, mygen);
    return 0;
}
//***  ControlledExitCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * ControlledExitCallback --
 *
 *      ngspice callback function invoked when ngspice requests a controlled shutdown,
 *      either due to a normal "quit" command or an internal unrecoverable error.
 *      Signals the Tcl-side logic that ngspice has exited, updates state flags,
 *      and queues a CONTROLLED_EXIT event for main-thread processing.
 *
 * Parameters:
 *      int status                     - input: exit status code provided by ngspice.
 *      bool immediate                 - input: if true, ngspice requests immediate unloading of the shared library.
 *      bool exit_upon_exit            - input: if true, indicates user-requested quit; otherwise, internal ngspice
 *                                              error.
 *      int id                         - input: identification number of the ngspice shared library instance. void
 *      user                           - input: user data pointer supplied during ngspice initialization, expected
 *                                              to be anNgSpiceContext.
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - Marks the ngspice context as exited (ctx->exited = 1) and clears ctx->quitting.
 *      - Signals ctx->exit_cv to wake any threads waiting for ngspice termination.
 *      - Increments the CONTROLLED_EXIT event counter via BumpAndSignal().
 *      - If ctx->destroying is false:
 *          - Captures the current generation counter (ctx->gen) under ctx->mutex.
 *          - Queues a CONTROLLED_EXIT Tcl event for deferred handling in the main thread.
 *      - Ensures proper synchronization using ctx->exit_mu and ctx->mutex.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */

int ControlledExitCallback(int status, bool immediate, bool exit_upon_exit, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx)
        return 0;

    Tcl_MutexLock(&ctx->exit_mu);
    ctx->exited = 1;
    ctx->quitting = 0;
    Tcl_ConditionNotify(&ctx->exit_cv);
    Tcl_MutexUnlock(&ctx->exit_mu);
    BumpAndSignal(ctx, CONTROLLED_EXIT);
    if (!ctx->destroying) {
        uint64_t mygen;
        Tcl_MutexLock(&ctx->mutex);
        mygen = ctx->gen;
        Tcl_MutexUnlock(&ctx->mutex);
        NgSpiceQueueEvent(ctx, CONTROLLED_EXIT, mygen);
    }
    return 0;
}
//***  SendDataCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendDataCallback --
 *
 *      ngspice callback function invoked whenever new simulation data (vector values)
 *      become available during analysis. Each invocation provides one or more sets of
 *      vector values that represent the current simulation point.
 *
 * Parameters:
 *      pvecvaluesall all              - input: pointer to a structure containing the current set of vectors and values.
 *      int count                      - input: number of vector sets provided (usually 1).
 *      int id                         - input: identification number of the calling ngspice shared library instance.
 *      void *user                     - input: user data pointer supplied during ngspice initialization,
 *                                        expected to be an NgSpiceContext *.
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid, count > 0, and ctx->destroying is false:
 *          - Allocates a new DataRow to hold the current vector values.
 *          - Copies each vector’s metadata and values into a new array of DataCell entries.
 *          - Appends the completed DataRow to ctx->prod (the producer data buffer) under ctx->mutex protection.
 *          - Increments the SEND_DATA event counter and signals any waiting threads via BumpAndSignal().
 *          - Queues a SEND_DATA Tcl event (NgSpiceQueueEvent) for deferred main-thread processing.
 *      - Ensures thread safety with ctx->mutex around shared buffer access.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int SendDataCallback(pvecvaluesall all, int count, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    uint64_t mygen;
    if (!ctx || !all || count <= 0 || ctx->destroying) {
        return 0;
    }
    DataRow row;
    row.veccount = all->veccount;
    row.vecs = (DataCell *)Tcl_Alloc(sizeof(DataCell) * row.veccount);
    for (int i = 0; i < row.veccount; i++) {
        pvecvalues v = all->vecsa[i];
        row.vecs[i].name = ckstrdup(v->name);
        row.vecs[i].is_complex = v->is_complex;
        row.vecs[i].creal = v->creal;
        row.vecs[i].cimag = v->cimag;
    }
    Tcl_MutexLock(&ctx->mutex);
    mygen = ctx->gen;
    DataBuf_Ensure(&ctx->prod, ctx->prod.count + 1);
    ctx->prod.rows[ctx->prod.count++] = row;
    Tcl_MutexUnlock(&ctx->mutex);
    BumpAndSignal(ctx, SEND_DATA);
    NgSpiceQueueEvent(ctx, SEND_DATA, mygen);
    return 0;
}
//***  SendInitDataCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendInitDataCallback --
 *
 *      ngspice callback function invoked once at the start of a simulation run,
 *      after the simulator has completed initialization and all vectors for the
 *      upcoming analysis have been defined. Captures metadata describing each vector
 *      (name, index, and type), resets the data buffer, and triggers a new generation.
 *
 * Parameters:
 *      pvecinfoall vinfo              - input: pointer to a structure containing information about all initialized
 *vectors. int id                         - input: identification number of the calling ngspice shared library instance.
 *      void *user                     - input: user data pointer supplied during ngspice initialization,
 *                                              expected to be an NgSpiceContext *.
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid and ctx->destroying is false:
 *          - Allocates a new InitSnap structure holding vector metadata (name, number, is_real flag).
 *          - Frees any previously stored initialization snapshot (ctx->init_snap) before replacing it.
 *          - Resets ctx->prod (the producer data buffer) to prepare for new simulation data.
 *          - Increments ctx->gen (the generation counter), marking a new run boundary.
 *          - Sets ctx->new_run_pending to request a data reset in NgSpiceEventProc.
 *          - Increments the SEND_INIT_DATA event counter and signals any waiting threads via BumpAndSignal().
 *          - Queues a SEND_INIT_DATA Tcl event (NgSpiceQueueEvent) for deferred main-thread processing.
 *      - Ensures thread safety by locking ctx->mutex during all shared state updates.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int SendInitDataCallback(pvecinfoall vinfo, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx || !vinfo || ctx->destroying) {
        return 0;
    }
    InitSnap *snap = (InitSnap *)Tcl_Alloc(sizeof *snap);
    snap->veccount = vinfo->veccount;
    snap->vecs = (typeof(snap->vecs))Tcl_Alloc(snap->veccount * sizeof *snap->vecs);
    for (int i = 0; i < snap->veccount; i++) {
        pvecinfo vec = vinfo->vecs[i];
        snap->vecs[i].name = ckstrdup(vec->vecname);
        snap->vecs[i].number = vec->number;
        snap->vecs[i].is_real = vec->is_real;
    }
    Tcl_MutexLock(&ctx->mutex);
    if (ctx->init_snap) {
        for (int i = 0; i < ctx->init_snap->veccount; i++) {
            Tcl_Free(ctx->init_snap->vecs[i].name);
        }
        Tcl_Free(ctx->init_snap->vecs);
        Tcl_Free(ctx->init_snap);
    }
    ctx->init_snap = snap;
    uint64_t mygen = ctx->gen;
    Tcl_MutexUnlock(&ctx->mutex);
    BumpAndSignal(ctx, SEND_INIT_DATA);
    NgSpiceQueueEvent(ctx, SEND_INIT_DATA, mygen);
    return 0;
}
//***  BGThreadRunningCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BGThreadRunningCallback --
 *
 *      libngspice callback that fires when the background simulation thread changes state.
 *      We record whether the background thread just started or ended, update internal
 *      bookkeeping, wake any waiters, and enqueue a Tcl event for the main thread.
 *
 * Parameters:
 *      bool running   - false means the background thread has just STARTED,
 *                       true  means the background thread has just STOPPED.
 *      int id         - ngspice instance ID (unused here).
 *      void *user     - NgSpiceContext * passed at ngSpice_Init() time.
 *
 * Results:
 *      Always returns 0 (ngspice ignores the return value).
 *
 * Side Effects:
 *      - If ctx is NULL or ctx->destroying is already set, we return immediately.
 *
 *      - Otherwise:
 *          * Take ctx->bg_mu and update ctx->bg_started / ctx->bg_ended:
 *                running == false:
 *                    - mark ctx->bg_started = 1
 *                    - if ctx->state == NGSTATE_STARTING_BG:
 *                        set ctx->state = NGSTATE_BG_ACTIVE
 *                        FlushPending(ctx)   ;# run any commands queued while starting
 *
 *                running == true:
 *                    - mark ctx->bg_ended = 1
 *                    - if ctx->state == NGSTATE_STOPPING_BG or NGSTATE_BG_ACTIVE:
 *                        set ctx->state = NGSTATE_IDLE
 *                        FlushPending(ctx)   ;# now safe to run deferred commands
 *
 *          * Signal ctx->bg_cv so threads waiting in WaitForBGStarted()/WaitForBGEnded()
 *            can make progress.
 *
 *          * Log a human-readable status line into ctx->msgq via QueueMsg(), e.g.:
 *                "# background thread running started"
 *                "# background thread running ended"
 *
 *          * Call BumpAndSignal(ctx, BG_THREAD_RUNNING) to:
 *                - increment the BG_THREAD_RUNNING counter in ctx->evt_counts[]
 *                - wake up anyone blocked in waitevent bg_running
 *
 *          * Snapshot ctx->gen under ctx->mutex and queue a BG_THREAD_RUNNING Tcl event
 *            (NgSpiceQueueEvent), so the main thread can service it later.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int BGThreadRunningCallback(bool running, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    uint64_t mygen;
    if (!ctx || ctx->destroying) {
        return 0;
    }
    Tcl_MutexLock(&ctx->bg_mu);
    if (!running) {
        if (!ctx->bg_started) {
            ctx->bg_started = 1;
            if (ctx->state == NGSTATE_STARTING_BG) {
                ctx->state = NGSTATE_BG_ACTIVE;
                FlushPending(ctx);
            }
        }
    } else {
        ctx->bg_ended = 1;
        if (ctx->state == NGSTATE_STOPPING_BG) {
            ctx->state = NGSTATE_IDLE;
            FlushPending(ctx);
        } else if (ctx->state == NGSTATE_BG_ACTIVE) {
            ctx->state = NGSTATE_IDLE;
            FlushPending(ctx);
        }
    }
    Tcl_ConditionNotify(&ctx->bg_cv);
    Tcl_MutexUnlock(&ctx->bg_mu);
    QueueMsg(ctx, running ? "# background thread running ended" : "# background thread running started");
    BumpAndSignal(ctx, BG_THREAD_RUNNING);
    Tcl_MutexLock(&ctx->mutex);
    mygen = ctx->gen;
    Tcl_MutexUnlock(&ctx->mutex);
    NgSpiceQueueEvent(ctx, BG_THREAD_RUNNING, mygen);
    return 0;
}
//***  WaitForBGStarted function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * WaitForBGStarted --
 *
 *      Waits until the ngspice background simulation thread has started, or until an optional timeout expires.
 *      Ensures that subsequent operations (e.g. event waiting or destruction) occur only after ngspice has
 *      fully initialized its background thread.
 *
 * Parameters:
 *      NgSpiceContext *ctx            - input: pointer to the NgSpiceContext structure.
 *      int timeout_ms                 - input: maximum time to wait in milliseconds (0 or negative for infinite wait).
 *
 * Results:
 *      None. Returns after the background thread has started or the timeout has elapsed.
 *
 * Side Effects:
 *      - Polls ngSpice_running() and waits on ctx->bg_cv until ctx->bg_started is set by BGThreadRunningCallback().
 *      - If timeout_ms > 0, the wait is bounded by the specified duration.
 *      - Uses Tcl_MutexLock/Tcl_ConditionWait to safely synchronize with the callback thread.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void WaitForBGStarted(NgSpiceContext *ctx, int timeout_ms) {
    if (!ctx->ngSpice_running) {
        Tcl_Sleep(10);
        return;
    }
    Tcl_Time deadline, now;
    int use_deadline = (timeout_ms > 0);
    if (use_deadline) {
        Tcl_GetTime(&deadline);
        long us = (long)timeout_ms * 1000;
        deadline.usec += us % 1000000;
        deadline.sec += us / 1000000 + deadline.usec / 1000000;
        deadline.usec %= 1000000;
    }
    Tcl_MutexLock(&ctx->bg_mu);
    while (!ctx->bg_started) {
        if (ctx->ngSpice_running() == 1) {
            ctx->bg_started = 1;
            break;
        }
        if (!use_deadline) {
            Tcl_ConditionWait(&ctx->bg_cv, &ctx->bg_mu, NULL);
        } else {
            Tcl_GetTime(&now);
            if (now.sec > deadline.sec || (now.sec == deadline.sec && now.usec >= deadline.usec))
                break;
            Tcl_Time rel = deadline;
            rel.sec -= now.sec;
            rel.usec -= now.usec;
            if (rel.usec < 0) {
                rel.usec += 1000000;
                rel.sec -= 1;
            }
            Tcl_ConditionWait(&ctx->bg_cv, &ctx->bg_mu, &rel);
        }
    }
    Tcl_MutexUnlock(&ctx->bg_mu);
}
//***  WaitForBGEnded function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * WaitForBGEnded --
 *
 *      Waits until the ngspice background simulation thread has finished running, or until an optional timeout expires.
 *      Ensures that cleanup or context destruction does not proceed while the background thread is still active.
 *
 * Parameters:
 *      NgSpiceContext *ctx            - input: pointer to the NgSpiceContext structure.
 *      int timeout_ms                 - input: maximum time to wait in milliseconds (0 or negative for infinite wait).
 *
 * Results:
 *      None. Returns after the background thread has ended or the timeout has elapsed.
 *
 * Side Effects:
 *      - Polls ngSpice_running() and waits on ctx->bg_cv until ctx->bg_ended is set by BGThreadRunningCallback().
 *      - If ngSpice_running() returns 0, marks ctx->bg_ended = 1 immediately.
 *      - Uses Tcl_MutexLock/Tcl_ConditionWait to synchronize with the callback thread.
 *      - If timeout_ms > 0, the wait is bounded by the specified duration.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void WaitForBGEnded(NgSpiceContext *ctx, int timeout_ms) {
    Tcl_Time deadline, now;
    int use_deadline = (timeout_ms > 0);
    if (use_deadline) {
        Tcl_GetTime(&deadline);
        long us = (long)timeout_ms * 1000;
        deadline.usec += us % 1000000;
        deadline.sec += us / 1000000 + deadline.usec / 1000000;
        deadline.usec %= 1000000;
    }
    Tcl_MutexLock(&ctx->bg_mu);
    while (!ctx->bg_ended) {
        if (!ctx->ngSpice_running || ctx->ngSpice_running() == 0) {
            ctx->bg_ended = 1;
            break;
        }
        if (!use_deadline) {
            Tcl_ConditionWait(&ctx->bg_cv, &ctx->bg_mu, NULL);
        } else {
            Tcl_GetTime(&now);
            if (now.sec > deadline.sec || (now.sec == deadline.sec && now.usec >= deadline.usec))
                break;
            Tcl_Time rel = deadline;
            rel.sec -= now.sec;
            rel.usec -= now.usec;
            if (rel.usec < 0) {
                rel.usec += 1000000;
                rel.sec -= 1;
            }
            Tcl_ConditionWait(&ctx->bg_cv, &ctx->bg_mu, &rel);
        }
    }
    Tcl_MutexUnlock(&ctx->bg_mu);
}

//** free functions
//***  InstFreeProc function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * InstFreeProc --
 *
 *      Final cleanup routine for an NgSpiceContext. This is the last stage of teardown for a simulator instance. It
 *      runs asynchronously after Tcl_EventuallyFree() decides that no Tcl events in the queue still reference this
 *      context.
 *
 *      IMPORTANT: InstFreeProc must only run after ngspice is fully quiesced and will never call back into us
 *      again. InstDeleteProc is responsible for:
 *          - stopping any background simulation activity,
 *          - synchronizing with ControlledExitCallback() so ctx->exited is set,
 *          - purging all queued NgSpiceEvent records for this ctx,
 *          - marking ctx->destroying = 1 and moving ctx->state to NGSTATE_DEAD,
 *          - detaching and freeing any deferred command queue.
 *
 *      By the time InstFreeProc executes, we assume:
 *          - no callbacks are still racing on this ctx,
 *          - no Tcl event in the system will try to access ctx,
 *          - ctx is no longer visible from Tcl,
 *          - and it is now safe to tear everything down for real.
 *
 * Parameters:
 *      void *cdata            - input: pointer to the NgSpiceContext being freed.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *
 *   1. Global poison guard.
 *      If g_heap_poisoned is nonzero, we assume libngspice went down in an unsafe or partially torn state earlier
 *      In that mode:
 *          - We return immediately without touching ctx.
 *          - We do NOT free any memory owned by ctx.
 *          - We do NOT attempt to dlclose() the ngspice handle.
 *
 *      Rationale: once the process is marked poisoned, libngspice (and anything it may have corrupted) is considered
 *      unsafe to poke. We intentionally leak ctx to avoid risking a crash in a compromised heap.
 *
 *   2. Wake any waitevent stragglers.
 *      As a last courtesy, we notify ctx->cond under ctx->mutex so any thread still stuck in [$s waitevent ...] can
 *      unblock. At this point the Tcl command is already gone, but this avoids leaving a thread permanently asleep
 *      on a dying context.
 *
 *   3. Release Tcl-managed objects.
 *      We drop our holds on refcounted Tcl objects stored in the context:
 *
 *          if (ctx->vectorData)  Tcl_DecrRefCount(ctx->vectorData);
 *          if (ctx->vectorInit)  Tcl_DecrRefCount(ctx->vectorInit);
 *
 *      After this, Tcl may free those objects if nothing else references them.
 *
 *   4. Free dynamic data structures owned by the context.
 *      We clean up everything we allocated for runtime data capture:
 *
 *          - MsgQ_Free(&ctx->msgq);
 *          - MsgQ_Free(&ctx->capq);
 *          - DataBuf_Free(&ctx->prod);
 *          - DataBuf_Free(&ctx->pend);
 *
 *      This releases any queued message strings and any buffered vector rows.
 *
 *   5. Tear down synchronization primitives. We finalize all mutexes and condition variables associated with this
 *      context so they are no longer usable:
 *
 *          Tcl_ConditionFinalize(&ctx->cond);
 *          Tcl_MutexFinalize(&ctx->mutex);
 *
 *          Tcl_ConditionFinalize(&ctx->exit_cv);
 *          Tcl_MutexFinalize(&ctx->exit_mu);
 *
 *          Tcl_ConditionFinalize(&ctx->bg_cv);
 *          Tcl_MutexFinalize(&ctx->bg_mu);
 *
 *          Tcl_MutexFinalize(&ctx->cmd_mu);
 *
 *      After this point, no thread should attempt to lock or wait on any of these.
 *
 *   6. Optionally unload libngspice.
 *      If ctx->handle is non-NULL, we attempt to close the dynamically loaded ngspice library handle with PDl_Close(),
 *      but ONLY if it is considered safe:
 *
 *          - We skip dlclose() if ctx->skip_dlclose is set on this instance, or if the
 *            global g_disable_dlclose is set.
 *
 *        Both flags are set when InstDeleteProc detects that ngspice shut down abruptly
 *        (background thread vanished without a clean "ended" callback, heap may be bad).
 *        In that case we never try to unload the shared lib because that could crash.
 *
 *   7. Free the context itself.
 *      Finally we release the NgSpiceContext struct with Tcl_Free(ctx). At this point
 *      there should be no remaining references to ctx anywhere in Tcl or in our own
 *      queued events, also reference counts of Tcl_Objs inside ctx must be zero.
 *
 *      NOTE: Once ctx is freed, absolutely nothing is allowed to touch it: no events,
 *      no callbacks, no mutex ops. This is guaranteed by the sequencing enforced in
 *      InstDeleteProc and by Tcl_EventuallyFree().
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void InstFreeProc(void *cdata) {
    NgSpiceContext *ctx = (NgSpiceContext *)cdata;
    if (g_heap_poisoned) {
        return;
    }
    Tcl_MutexLock(&ctx->mutex);
    Tcl_ConditionNotify(&ctx->cond);
    Tcl_MutexUnlock(&ctx->mutex);
    if (ctx->vectorData) {
        Tcl_DecrRefCount(ctx->vectorData);
    }
    if (ctx->vectorInit) {
        Tcl_DecrRefCount(ctx->vectorInit);
    }
    MsgQ_Free(&ctx->msgq);
    MsgQ_Free(&ctx->capq);
    DataBuf_Free(&ctx->prod);
    DataBuf_Free(&ctx->pend);
    Tcl_ConditionFinalize(&ctx->cond);
    Tcl_MutexFinalize(&ctx->mutex);
    Tcl_ConditionFinalize(&ctx->exit_cv);
    Tcl_MutexFinalize(&ctx->exit_mu);
    Tcl_ConditionFinalize(&ctx->bg_cv);
    Tcl_MutexFinalize(&ctx->bg_mu);
    Tcl_MutexFinalize(&ctx->cmd_mu);
    if (ctx->handle) {
        if (ctx->skip_dlclose || g_disable_dlclose) {
        } else {
            PDl_Close(ctx->handle);
        }
    }
    //PrintRefCount(ctx->vectorData);
    //PrintRefCount(ctx->vectorInit);
    Tcl_Free(ctx);
}
//***  InstDeleteProc function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * InstDeleteProc --
 *
 *      Instance deletion procedure for an NgSpiceContext. Called when the Tcl command that represents a simulator
 *      instance is deleted (for example, via `$s1 destroy`). This function tries to shut down ngspice in a controlled
 *      way, stop further callbacks, clean up any pending state, and (if safe) schedule final cleanup of the instance.
 *
 *      This function also enforces a global "poison" mode. If ngspice has ever died in a partially torn-down / unsafe
 *      state (which can corrupt libngspice’s heap), we mark the process as contaminated by setting global flags. Once
 *      the process is poisoned, we never call back into ngspice again, never dlclose() its handle, and we leak the
 *      context on purpose instead of risking a crash.
 *
 * Parameters:
 *      void *cdata
 *          Pointer to the NgSpiceContext for this Tcl instance.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *
 *   Global poison fast path:
 *      If g_heap_poisoned is already set when this runs, we assume the process is already contaminated from a previous
 *      ngspice failure. In that case:
 *
 *          * We mark ctx->destroying = 1 so no new callbacks enqueue data or events.
 *
 *          * We tear down any deferred ngspice commands immediately and in a thread-safe way:
 *                - Lock ctx->cmd_mu, detach ctx->pending_head/tail.
 *                - Free all PendingCmd nodes outside the lock.
 *            This prevents any queued commands from being flushed later into a dead ngspice.
 *
 *          * We wake any thread currently blocked in waitevent:
 *                - Lock ctx->mutex, Tcl_ConditionNotify(&ctx->cond), unlock.
 *
 *          * We DO NOT:
 *                - call QuiesceNgspice()
 *                - call ctx->ngSpice_Command("quit")
 *                - wait on ctx->exit_cv
 *                - queue InstFreeProc()
 *                - dlclose() the ngspice handle
 *
 *      We just return. The context (and possibly libngspice itself) is intentionally leaked because touching it could
 *      be unsafe.
 *
 *   Normal shutdown path (when not already poisoned):
 *
 *      1. Stop further activity.
 *         - Set ctx->destroying = 1 so callbacks know to stop queuing data/events.
 *         - Force the high-level state machine into NGSTATE_DEAD under bg_mu, so future
 *           InstObjCmd calls will reject immediately.
 *
 *      2. Drain and discard any deferred commands.
 *         - Lock ctx->cmd_mu, detach the pending command list (pending_head/tail = NULL),
 *           unlock, then free all PendingCmd nodes.
 *           This guarantees nothing later will FlushPending() into ngspice after teardown
 *           has begun.
 *
 *      3. Observe background thread state.
 *         - Call WaitForBGStarted(ctx, 250) to latch whether ngspice ever told us the background thread "started"
 *           and/or "ended".
 *         - Snapshot:
 *               started = ctx->bg_started
 *               ended   = ctx->bg_ended
 *               running_now = ctx->ngSpice_running() (if available)
 *
 *      4. Detect an unsafe/abrupt shutdown.
 *         - We consider it "abrupt" only if:
 *               (started == 1) AND
 *               (running_now == 0) AND
 *               (ended == 0)
 *           That means ngspice’s bg thread used to exist, but we never got the clean "ended" callback, so we assume the
 *           lib may be in a half-dead state.
 *
 *         - In that case:
 *               abrupt_shutdown = 1;
 *               g_heap_poisoned = 1;
 *               g_disable_dlclose = 1;
 *               ctx->skip_dlclose = 1;
 *
 *           From now on the entire process is considered poisoned and we will refuse to touch ngspice internals or
 *           unload the library in any instance.
 *
 *      5. Halt the background thread if it's still legitimately running.
 *         - If (started && !ended && !abrupt_shutdown):
 *               QuiesceNgspice(ctx, 0);
 *               WaitForBGEnded(ctx, 3000);
 *           Otherwise we just mark ctx->bg_ended = 1 under bg_mu so later code won't spin.
 *
 *      6. Decide whether to tell ngspice to exit.
 *         - We only send "quit" if it still looks safe:
 *               safe_to_quit = !ctx->exited && !abrupt_shutdown
 *           and we have a valid ctx->ngSpice_Command and we haven't already sent it.
 *
 *         - Safe case:
 *               ctx->quitting = 1;
 *               ctx->ngSpice_Command("unset askquit");
 *               ctx->ngSpice_Command("quit");
 *
 *         - Unsafe case (either already exited, or abrupt_shutdown, or otherwise not safe):
 *               We do NOT call into ngspice.
 *               If ctx->exited is still 0, we fake a clean exit:
 *                   Lock ctx->exit_mu;
 *                   ctx->exited = 1;
 *                   Tcl_ConditionNotify(&ctx->exit_cv);
 *                   Unlock ctx->exit_mu;
 *               We also set ctx->skip_dlclose = 1 so InstFreeProc will not dlclose()
 *               a possibly-corrupted library.
 *
 *      7. Sync with exit.
 *         - We now wait until ctx->exited is definitely 1:
 *               Lock ctx->exit_mu;
 *               while (!ctx->exited) Tcl_ConditionWait(&ctx->exit_cv, ...);
 *               Unlock ctx->exit_mu;
 *
 *           In the normal path, ControlledExitCallback() will eventually mark exited=1 and signal exit_cv. In the
 *           unsafe/fake-exit path above, we already forced exited=1 and notified, so this wait returns immediately.
 *
 *      8. Purge any queued Tcl events for this context.
 *         - Tcl_DeleteEvents(DeleteNgSpiceEventProc, ctx) walks the Tcl event queue, drops all NgSpiceEvent entries
 *           that still refer to this ctx, and balances their Tcl_Preserve/Tcl_Release.
 *           After this point, no pending NgSpiceEventProc will ever run on a freed ctx.
 *
 *      9. Wake waitevent callers.
 *         - Lock ctx->mutex;
 *           Tcl_ConditionNotify(&ctx->cond);
 *           Unlock ctx->mutex;
 *
 *         This unblocks anyone in [$s waitevent ...] so they don't hang forever on an instance that is going away.
 *
 *     10. Final disposition.
 *         - If we just transitioned into poisoned mode (abrupt_shutdown set the globals), we stop here and return
 *           without scheduling cleanup. We intentionally leak ctx because we consider libngspice unsafe to touch
 *           further.
 *
 *         - Otherwise, we hand ownership to Tcl for orderly teardown:
 *               Tcl_EventuallyFree(ctx, InstFreeProc);
 *           InstFreeProc will run later (once no events still reference ctx) and will:
 *               * release Tcl objects (vectorData, vectorInit),
 *               * free message/data buffers,
 *               * finalize mutexes/conditions,
 *               * dlclose() libngspice unless skip_dlclose or g_disable_dlclose is set.
 *
 *   Idempotency:
 *      Calling InstDeleteProc multiple times is safe. If ctx->destroying was already set, or if the process is already
 *      poisoned, we just do minimal signaling / pending command drain and return. We don't try to shut ngspice down
 *      twice.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void InstDeleteProc(void *cdata) {
    NgSpiceContext *ctx = (NgSpiceContext *)cdata;
    if (g_heap_poisoned) {
        ctx->destroying = 1;
        Tcl_MutexLock(&ctx->cmd_mu);
        PendingCmd *plist = ctx->pending_head;
        ctx->pending_head = NULL;
        ctx->pending_tail = NULL;
        Tcl_MutexUnlock(&ctx->cmd_mu);
        while (plist) {
            PendingCmd *next = plist->next;
            Tcl_Free(plist->cmd);
            Tcl_Free(plist);
            plist = next;
        }
        /* Unblock any waitevent callers that might still be watching this ctx. */
        Tcl_MutexLock(&ctx->mutex);
        Tcl_ConditionNotify(&ctx->cond);
        Tcl_MutexUnlock(&ctx->mutex);
        return;
    }
    if (ctx->destroying) {
        return;
    }
    /* tell callbacks to stop enqueueing new work */
    ctx->destroying = 1;
    Tcl_MutexLock(&ctx->bg_mu);
    ctx->state = NGSTATE_DEAD;
    Tcl_MutexUnlock(&ctx->bg_mu);
    Tcl_MutexLock(&ctx->cmd_mu);
    /* free commands queue */
    PendingCmd *plist = ctx->pending_head;
    ctx->pending_head = NULL;
    ctx->pending_tail = NULL;
    Tcl_MutexUnlock(&ctx->cmd_mu);
    while (plist) {
        PendingCmd *next = plist->next;
        Tcl_Free(plist->cmd);
        Tcl_Free(plist);
        plist = next;
    }
    /* Step 1: observe early bg thread state */
    WaitForBGStarted(ctx, 250);
    Tcl_MutexLock(&ctx->bg_mu);
    int started = ctx->bg_started;
    int ended = ctx->bg_ended;
    Tcl_MutexUnlock(&ctx->bg_mu);
    int running_now = -1;
    if (ctx->ngSpice_running) {
        running_now = ctx->ngSpice_running();
    }
    int abrupt_shutdown = 0;
    if (started && (running_now == 0) && !ended) {
        abrupt_shutdown = 1;
        g_heap_poisoned = 1;
        g_disable_dlclose = 1;
        ctx->skip_dlclose = 1;
    }
    /* Step 2: try graceful halt if bg thread might still be legitimately running, and we didn't detect the abrupt
       case. */
    if (started && !ended && !abrupt_shutdown) {
        QuiesceNgspice(ctx, 0);
        WaitForBGEnded(ctx, 3000);
    } else {
        Tcl_MutexLock(&ctx->bg_mu);
        ctx->bg_ended = 1;
        Tcl_MutexUnlock(&ctx->bg_mu);
    }
    /* Snapshot after shutdown attempts */
    Tcl_MutexLock(&ctx->bg_mu);
    ended = ctx->bg_ended;
    Tcl_MutexUnlock(&ctx->bg_mu);
    running_now = (ctx->ngSpice_running ? ctx->ngSpice_running() : -1);
    /* Step 3: decide whether to call "quit" */
    int safe_to_quit = 1;
    if (ctx->exited) {
        safe_to_quit = 0;
    }
    if (abrupt_shutdown) {
        safe_to_quit = 0;
    }
    if (safe_to_quit && ctx->ngSpice_Command && !ctx->quitting) {
        ctx->quitting = 1;
        ctx->ngSpice_Command("unset askquit");
        ctx->ngSpice_Command("quit");
    } else if (!safe_to_quit && !ctx->exited) {
        Tcl_MutexLock(&ctx->exit_mu);
        ctx->exited = 1;
        Tcl_ConditionNotify(&ctx->exit_cv);
        Tcl_MutexUnlock(&ctx->exit_mu);
        ctx->skip_dlclose = 1;
    }
    /* Step 4: sync with ControlledExitCallback OR our fake exit */
    Tcl_MutexLock(&ctx->exit_mu);
    while (!ctx->exited) {
        Tcl_ConditionWait(&ctx->exit_cv, &ctx->exit_mu, NULL);
    }
    Tcl_MutexUnlock(&ctx->exit_mu);
    Tcl_DeleteEvents(DeleteNgSpiceEventProc, ctx);
    Tcl_MutexLock(&ctx->mutex);
    Tcl_ConditionNotify(&ctx->cond);
    Tcl_MutexUnlock(&ctx->mutex);
    if (g_heap_poisoned) {
        ctx->destroying = 1;
        Tcl_MutexLock(&ctx->mutex);
        Tcl_ConditionNotify(&ctx->cond);
        Tcl_MutexUnlock(&ctx->mutex);
        return;
    }
    Tcl_EventuallyFree((ClientData)ctx, InstFreeProc);
}
//** command registering function
//***  InstObjCmd function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * InstObjCmd --
 *
 *      Object command procedure for a single ngspice bridge instance. This is the Tcl command that scripts call
 *      (e.g. "::ngspicetclbridge::s1 ..."). Each subcommand here operates on the same NgSpiceContext and its associated
 *      libngspice state.
 *
 * Parameters:
 *      ClientData cdata      - pointer to the NgSpiceContext for this instance
 *      Tcl_Interp *interp    - target interpreter
 *      Tcl_Size objc         - number of arguments
 *      Tcl_Obj *const objv[] - argv-style list of Tcl_Objs:
 *                                  objv[0] = instance command name
 *                                  objv[1] = subcommand
 *                                  objv[2+] = subcommand args (if any)
 *
 * Results:
 *      TCL_OK or TCL_ERROR. On success, some subcommands also set a Tcl result.
 *
 * Supported Subcommands:
 *
 *
 *   command ?-capture? string
 *      - Sends an ngspice command via ngSpice_Command().
 *      - Normal form: "command <str>"
 *            -> just call ngSpice_Command(), result is its int return code.
 *      - Capture form: "command -capture <str>"
 *            -> clear ctx->capq, enable ctx->cap_active, run the command, disable capture, return dict {rc <int> output
 *               <list-of-lines>}.
 *      - Special handling for bg_run/bg_halt to manage ctx->state and to defer commands while the background thread is
 *        starting or stopping.
 *      - While ctx->state is NGSTATE_STARTING_BG or NGSTATE_STOPPING_BG, commands are queued (EnqueuePending) instead
 *        of executed immediately.
 *
 *   circuit list
 *      - Sends a full circuit deck to ngSpice_Circ(), building a transient NULL-terminated char** from the Tcl list.
 *      - Result: int rc from ngSpice_Circ().
 *
 *   waitevent name ?-n N? ?timeout_ms?
 *      - Blocks until the given event fires N more times (default N=1), or until timeout_ms expires (default: no
 *        timeout).
 *      - Valid event names:
 *          send_char, send_stat, controlled_exit, send_data, send_init_data, bg_running.
 *      - Returns a dict:
 *          fired   <bool>           (1 if condition met)
 *          count   <int64>          (cumulative count for that event)
 *          need    <int64>          (requested N)
 *          status  ok|timeout|aborted
 *        where "aborted" means ctx->aborting or ctx->destroying tripped while waiting.
 *      - Internally uses wait_for() on ctx->evt_counts[].
 *
 *   vectors ?-clear?
 *      - Without -clear: returns ctx->vectorData (dict: vecName -> list-of-samples).
 *      - With -clear: replaces ctx->vectorData with a new empty dict and returns nothing.
 *
 *   plot
 *   plot -all
 *   plot -vecs plotname
 *      - "plot": returns current plot name (ngSpice_CurPlot()).
 *      - "plot -all": returns list of all plot names (ngSpice_AllPlots()).
 *      - "plot -vecs <plot>": returns list of vector names in that plot (ngSpice_AllVecs()).
 *      - Errors if options don't match.
 *
 *   asyncvector name
 *   asyncvector -info name
 *      - asyncvector <name>:
 *            * Queries ngGet_Vec_Info(<name>), returns list of data samples.
 *            * Complex data is returned as {real imag} pairs.
 *      - asyncvector -info <name>:
 *            * Returns dict with metadata:
 *                type    (physical/sweep meaning)
 *                length  (#samples)
 *                ntype   ("real" or "complex")
 *            * Errors if vector doesn't exist.
 *
 *   isrunning
 *      - Calls ngSpice_running() and returns a boolean:
 *            1 if ngspice bg thread is active,
 *            0 otherwise.
 *      - If ctx->destroying is already true, we skip calling into ngspice.
 *
 *   initvectors ?-clear?
 *      - Without -clear: returns ctx->vectorInit, a dict captured at run start:
 *            vecName -> {number <int> real <bool>}
 *      - With -clear: replaces ctx->vectorInit with a new empty dict.
 *
 *   messages ?-clear?
 *      - Without -clear: returns Tcl list of all queued ngspice output/status lines
 *        (from SendCharCallback, SendStatCallback, BGThreadRunningCallback, ControlledExitCallback, etc.).
 *      - With -clear: clears ctx->msgq and returns nothing.
 *
 *   eventcounts ?-clear?
 *      - Without -clear: returns dict of cumulative callback counters:
 *            send_char, send_stat, controlled_exit, send_data, send_init_data, bg_running.
 *      - With -clear: zeros ctx->evt_counts[].
 *
 *   destroy
 *      - Deletes this Tcl command, which triggers InstDeleteProc(): stops bg thread, asks ngspice to quit, waits for
 *          shutdown, purges events, and schedules InstFreeProc().
 *
 *   abort
 *      - Forces any current waitevent to unblock early:
 *          sets ctx->aborting=1 and signals ctx->cond.
 *      - Does NOT destroy the instance or stop ngspice.
 *
 * Notes / Side Effects:
 *      - Most subcommands grab ctx->mutex and related sync primitives.
 *      - We call directly into libngspice (ctx->ngSpice_*).
 *      - We manage Tcl refcounts on ctx->vectorData / ctx->vectorInit.
 *      - "waitevent" may block while waiting for ngspice callbacks.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int InstObjCmd(ClientData cdata, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    int code = TCL_OK;
    NgSpiceContext *ctx = (NgSpiceContext *)cdata;
    Tcl_Preserve((ClientData)ctx);
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?args?");
        code = TCL_ERROR;
        goto done;
    }
    const char *sub = Tcl_GetString(objv[1]);
    if (strcmp(sub, "command") == 0) {
        int do_capture = 0;
        int argi = 2;
        if (objc == 4) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-capture") == 0) {
                do_capture = 1;
                argi = 3;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -capture)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-capture? string");
            code = TCL_ERROR;
            goto done;
        }
        const char *cmd = Tcl_GetString(objv[argi]);
        Tcl_MutexLock(&ctx->bg_mu);
        NgState st = ctx->state;
        Tcl_MutexUnlock(&ctx->bg_mu);
        if (st == NGSTATE_DEAD || ctx->destroying) {
            Tcl_SetResult(interp, "instance is shutting down", TCL_STATIC);
            code = TCL_ERROR;
            goto done;
        }
        if (st == NGSTATE_STARTING_BG) {
            EnqueuePending(ctx, cmd, do_capture);
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("background thread is starting, command %s is deffered", cmd));
            code = TCL_OK;
            goto done;
        }
        if (st == NGSTATE_STOPPING_BG) {
            EnqueuePending(ctx, cmd, do_capture);
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("background thread is stopping, command %s is deffered", cmd));
            code = TCL_OK;
            goto done;
        }
        if (strcmp(cmd, "bg_run") == 0) {
            Tcl_MutexLock(&ctx->bg_mu);
            ctx->state = NGSTATE_STARTING_BG;
            ctx->bg_started = 0;
            ctx->bg_ended = 0;
            Tcl_MutexUnlock(&ctx->bg_mu);
            Tcl_MutexLock(&ctx->mutex);
            ctx->gen++;
            ctx->new_run_pending = 0;
            if (ctx->init_snap) {
                for (int i = 0; i < ctx->init_snap->veccount; i++) {
                    Tcl_Free(ctx->init_snap->vecs[i].name);
                }
                Tcl_Free(ctx->init_snap->vecs);
                Tcl_Free(ctx->init_snap);
                ctx->init_snap = NULL;
            }
            DataBuf_Free(&ctx->prod);
            DataBuf_Init(&ctx->prod);
            if (ctx->vectorData) {
                Tcl_DecrRefCount(ctx->vectorData);
            }
            ctx->vectorData = Tcl_NewDictObj();
            Tcl_IncrRefCount(ctx->vectorData);
            if (ctx->vectorInit) {
                Tcl_DecrRefCount(ctx->vectorInit);
            }
            ctx->vectorInit = Tcl_NewDictObj();
            Tcl_IncrRefCount(ctx->vectorInit);
            Tcl_MutexUnlock(&ctx->mutex);
        }
        if (strcmp(cmd, "bg_halt") == 0) {
            Tcl_MutexLock(&ctx->bg_mu);
            if (ctx->state == NGSTATE_BG_ACTIVE) {
                ctx->state = NGSTATE_STOPPING_BG;
            } else {
            }
            Tcl_MutexUnlock(&ctx->bg_mu);
        }
        if (!do_capture) {
            int rc = ctx->ngSpice_Command((char *)cmd);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
            code = TCL_OK;
            goto done;
        } else {
            Tcl_MutexLock(&ctx->mutex);
            MsgQ_Clear(&ctx->capq);
            ctx->cap_active = 1;
            Tcl_MutexUnlock(&ctx->mutex);
            int rc = ctx->ngSpice_Command((char *)cmd);
            Tcl_Obj *outList = Tcl_NewListObj(0, NULL);
            Tcl_MutexLock(&ctx->mutex);
            ctx->cap_active = 0;
            for (size_t i = 0; i < ctx->capq.count; i++) {
                Tcl_ListObjAppendElement(interp, outList, Tcl_NewStringObj(ctx->capq.items[i], -1));
            }
            MsgQ_Clear(&ctx->capq);
            Tcl_MutexUnlock(&ctx->mutex);
            Tcl_Obj *res = Tcl_NewDictObj();
            Tcl_DictObjPut(interp, res, Tcl_NewStringObj("rc", -1), Tcl_NewIntObj(rc));
            Tcl_DictObjPut(interp, res, Tcl_NewStringObj("output", -1), outList);
            Tcl_SetObjResult(interp, res);
            code = TCL_OK;
            goto done;
        }
    }
    if (strcmp(sub, "circuit") == 0) {
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "list");
            code = TCL_ERROR;
            goto done;
        }
        Tcl_Size cirLinesListLen;
        Tcl_Obj **cirLinesElems;
        if (Tcl_ListObjGetElements(interp, objv[2], &cirLinesListLen, &cirLinesElems) != TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("error getting circuit list", -1));
            code = TCL_ERROR;
            goto done;
        }
        if (ctx->has_circuit) {
            ctx->ngSpice_Command("remcirc");
            ctx->has_circuit = 0;
        }
        char **circuit = Tcl_Alloc((cirLinesListLen + 1) * sizeof(char *));
        for (Tcl_Size i = 0; i < cirLinesListLen; ++i) {
            circuit[i] = Tcl_GetString(cirLinesElems[i]);
        }
        circuit[cirLinesListLen] = NULL;
        int rc = ctx->ngSpice_Circ(circuit);
        ctx->has_circuit = 1;
        Tcl_Free(circuit);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "inputpath") == 0) {
        char *resPath;
        if (objc == 3) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-current") == 0) {
                resPath = ctx->ngCM_Input_Path(NULL);
                Tcl_SetObjResult(interp, Tcl_NewStringObj(resPath, -1));
                code = TCL_OK;
                goto done;
            } else {
                resPath = ctx->ngCM_Input_Path(opt);
                Tcl_SetObjResult(interp, Tcl_NewStringObj(resPath, -1));
                code = TCL_OK;
                goto done;
            }
        } else {
            Tcl_WrongNumArgs(interp, 2, objv, "-current|path");
            code = TCL_ERROR;
            goto done;
        }
    }
    if (strcmp(sub, "waitevent") == 0) {
        int which;
        uint64_t need = 1;
        long timeout_ms = 0;
        int i = 2;
        if (objc <= i) {
            Tcl_WrongNumArgs(interp, 2, objv, "name ?-n N? ?timeout_ms?");
            code = TCL_ERROR;
            goto done;
        }
        if (NameToEvtId(objv[i++], &which) != TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown event: %s", Tcl_GetString(objv[i - 1])));
            code = TCL_ERROR;
            goto done;
        }
        if (i < objc && strcmp(Tcl_GetString(objv[i]), "-n") == 0) {
            if (i + 1 >= objc || Tcl_GetWideIntFromObj(interp, objv[i + 1], (Tcl_WideInt *)&need) != TCL_OK ||
                need < 1) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj("expected positive integer after -n", -1));
                code = TCL_ERROR;
                goto done;
            }
            i += 2;
        }
        if (i < objc) {
            if (Tcl_GetLongFromObj(interp, objv[i], &timeout_ms) != TCL_OK) {
                code = TCL_ERROR;
                goto done;
            }
            i++;
        }
        if (i != objc) {
            Tcl_WrongNumArgs(interp, 2, objv, "name ?-n N? ?timeout_ms?");
            code = TCL_ERROR;
            goto done;
        }
        int reached;
        uint64_t count;
        ctx->aborting = 0;
        wait_rc rc = wait_for(ctx, which, need, timeout_ms, &reached, &count);
        Tcl_Obj *res = Tcl_NewDictObj();
        Tcl_DictObjPut(interp, res, Tcl_NewStringObj("fired", -1), Tcl_NewBooleanObj(reached));
        Tcl_DictObjPut(interp, res, Tcl_NewStringObj("count", -1), Tcl_NewWideIntObj((Tcl_WideInt)count));
        Tcl_DictObjPut(interp, res, Tcl_NewStringObj("need", -1), Tcl_NewWideIntObj((Tcl_WideInt)need));
        Tcl_DictObjPut(interp, res, Tcl_NewStringObj("status", -1),
                       Tcl_NewStringObj(rc == NGSPICE_WAIT_OK        ? "ok"
                                        : rc == NGSPICE_WAIT_TIMEOUT ? "timeout"
                                                                     : "aborted",
                                        -1));
        Tcl_SetObjResult(interp, res);
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "vectors") == 0) {
        int do_clear = 0;
        if (objc == 3) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-clear") == 0) {
                do_clear = 1;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -clear)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-clear?");
            code = TCL_ERROR;
            goto done;
        }
        if (!ctx->vectorData) {
            Tcl_SetResult(interp, "no vector data", TCL_STATIC);
            code = TCL_ERROR;
            goto done;
        }
        Tcl_MutexLock(&ctx->mutex);
        if (do_clear) {
            Tcl_DecrRefCount(ctx->vectorData);
            ctx->vectorData = Tcl_NewDictObj();
            Tcl_IncrRefCount(ctx->vectorData);
            Tcl_MutexUnlock(&ctx->mutex);
            code = TCL_OK;
            goto done;
        } else {
            Tcl_SetObjResult(interp, ctx->vectorData);
            Tcl_MutexUnlock(&ctx->mutex);
            code = TCL_OK;
            goto done;
        }
    }
    if (strcmp(sub, "plot") == 0) {
        if (objc == 2) {
            Tcl_Obj *currentPlot = Tcl_NewStringObj(ctx->ngSpice_CurPlot(), -1);
            Tcl_SetObjResult(interp, currentPlot);
            code = TCL_OK;
            goto done;
        } else if (objc == 3) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-all") == 0) {
                char **plots = ctx->ngSpice_AllPlots();
                Tcl_Obj *plotsNamesList = Tcl_NewListObj(0, NULL);
                for (Tcl_Size i = 0; plots[i] != NULL; ++i) {
                    Tcl_ListObjAppendElement(interp, plotsNamesList, Tcl_NewStringObj(plots[i], -1));
                }
                Tcl_SetObjResult(interp, plotsNamesList);
                code = TCL_OK;
                goto done;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -all)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc == 4) {
            const char *opt = Tcl_GetString(objv[2]);
            char *arg = Tcl_GetString(objv[3]);
            if (strcmp(opt, "-vecs") == 0) {
                char **vecsNames = ctx->ngSpice_AllVecs(arg);
                Tcl_Obj *vecsNamesList = Tcl_NewListObj(0, NULL);
                for (Tcl_Size i = 0; vecsNames[i] != NULL; ++i) {
                    Tcl_ListObjAppendElement(interp, vecsNamesList, Tcl_NewStringObj(vecsNames[i], -1));
                }
                Tcl_SetObjResult(interp, vecsNamesList);
                code = TCL_OK;
                goto done;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -vecs)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else {
            Tcl_WrongNumArgs(interp, 2, objv, "?-all?|?-vecs plotname?");
            code = TCL_ERROR;
            goto done;
        }
    }
    if (strcmp(sub, "asyncvector") == 0) {
        if (objc == 4) {
            const char *opt = Tcl_GetString(objv[2]);
            const char *vecname = Tcl_GetString(objv[3]);
            if (strcmp(opt, "-info") == 0) {
                ctx->ngSpice_LockRealloc();
                pvector_info vinfo = ctx->ngGet_Vec_Info((char *)vecname);
                if (vinfo == NULL) {
                    Tcl_Obj *errMsg = Tcl_ObjPrintf("vector with name \"%s\" does not exist", vecname);
                    Tcl_SetObjResult(interp, errMsg);
                    code = TCL_ERROR;
                    goto done;
                }
                int vlength = vinfo->v_length;
                int vtype = vinfo->v_type;
                Tcl_Obj *info = Tcl_NewDictObj();
                Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("notype", -1));
                switch ((enum vector_types)vtype) {
                case SV_NOTYPE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("notype", -1));
                    break;
                case SV_TIME:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("time", -1));
                    break;
                case SV_FREQUENCY:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("frequency", -1));
                    break;
                case SV_VOLTAGE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("voltage", -1));
                    break;
                case SV_CURRENT:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("current", -1));
                    break;
                case SV_VOLTAGE_DENSITY:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("voltage-density", -1));
                    break;
                case SV_CURRENT_DENSITY:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("current-density", -1));
                    break;
                case SV_SQR_VOLTAGE_DENSITY:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1),
                                   Tcl_NewStringObj("voltage^2-density", -1));
                    break;
                case SV_SQR_CURRENT_DENSITY:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1),
                                   Tcl_NewStringObj("current^2-density", -1));
                    break;
                case SV_SQR_VOLTAGE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("temperature", -1));
                    break;
                case SV_SQR_CURRENT:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("charge", -1));
                    break;
                case SV_POLE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("pole", -1));
                    break;
                case SV_ZERO:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("zero", -1));
                    break;
                case SV_SPARAM:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("s-param", -1));
                    break;
                case SV_TEMP:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("temp-sweep", -1));
                    break;
                case SV_RES:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("res-sweep", -1));
                    break;
                case SV_IMPEDANCE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("impedance", -1));
                    break;
                case SV_ADMITTANCE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("admittance", -1));
                    break;
                case SV_POWER:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("power", -1));
                    break;
                case SV_PHASE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("phase", -1));
                    break;
                case SV_DB:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("decibel", -1));
                    break;
                case SV_CAPACITANCE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("capacitance", -1));
                    break;
                case SV_CHARGE:
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("charge", -1));
                    break;
                };
                Tcl_DictObjPut(interp, info, Tcl_NewStringObj("length", -1), Tcl_NewIntObj(vlength));
                if (vinfo->v_flags & VF_COMPLEX) {
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("ntype", -1), Tcl_NewStringObj("complex", -1));
                } else {
                    Tcl_DictObjPut(interp, info, Tcl_NewStringObj("ntype", -1), Tcl_NewStringObj("real", -1));
                }
                ctx->ngSpice_UnlockRealloc();
                Tcl_SetObjResult(interp, info);
                code = TCL_OK;
                goto done;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -info)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc == 3) {
            const char *vecname = Tcl_GetString(objv[2]);
            ctx->ngSpice_LockRealloc();
            pvector_info vinfo = ctx->ngGet_Vec_Info((char *)vecname);
            if (vinfo == NULL) {
                Tcl_Obj *errMsg = Tcl_ObjPrintf("vector with name \"%s\" does not exist", vecname);
                ctx->ngSpice_UnlockRealloc();
                Tcl_SetObjResult(interp, errMsg);
                code = TCL_ERROR;
                goto done;
            }
            int vlength = vinfo->v_length;
            Tcl_Obj *dataObj = Tcl_NewListObj(0, NULL);
            if (vinfo->v_flags & VF_COMPLEX) {
                ngcomplex_t *cdata = vinfo->v_compdata;
                for (int i = 0; i < vlength; i++) {
                    Tcl_Obj *pair = Tcl_NewListObj(0, NULL);
                    Tcl_ListObjAppendElement(interp, pair, Tcl_NewDoubleObj(cdata[i].cx_real));
                    Tcl_ListObjAppendElement(interp, pair, Tcl_NewDoubleObj(cdata[i].cx_imag));
                    Tcl_ListObjAppendElement(interp, dataObj, pair);
                }
            } else {
                double *rdata = vinfo->v_realdata;
                for (int i = 0; i < vlength; i++) {
                    Tcl_ListObjAppendElement(interp, dataObj, Tcl_NewDoubleObj(rdata[i]));
                }
            }
            ctx->ngSpice_UnlockRealloc();
            Tcl_SetObjResult(interp, dataObj);
            code = TCL_OK;
            goto done;
        } else {
            Tcl_WrongNumArgs(interp, 2, objv, "string");
            code = TCL_ERROR;
            goto done;
        }
    }
    if (strcmp(sub, "isrunning") == 0) {
        if (objc > 2) {
            Tcl_WrongNumArgs(interp, 1, objv, NULL);
            code = TCL_ERROR;
            goto done;
        }
        if (ctx->destroying) {
            code = TCL_OK;
            goto done;
        }
        bool isrunning = ctx->ngSpice_running();
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(isrunning));
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "initvectors") == 0) {
        int do_clear = 0;
        if (objc == 3) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-clear") == 0) {
                do_clear = 1;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -clear)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-clear?");
            code = TCL_ERROR;
            goto done;
        }
        if (!ctx->vectorInit) {
            Tcl_SetResult(interp, "no init vector data", TCL_STATIC);
            code = TCL_ERROR;
            goto done;
        }
        Tcl_MutexLock(&ctx->mutex);
        if (do_clear) {
            Tcl_DecrRefCount(ctx->vectorInit);
            ctx->vectorInit = Tcl_NewDictObj();
            Tcl_IncrRefCount(ctx->vectorInit);
            Tcl_MutexUnlock(&ctx->mutex);
            code = TCL_OK;
            goto done;
        } else {
            Tcl_SetObjResult(interp, ctx->vectorInit);
            Tcl_MutexUnlock(&ctx->mutex);
            code = TCL_OK;
            goto done;
        }
    }
    if (strcmp(sub, "messages") == 0) {
        int do_clear = 0;
        if (objc == 3) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-clear") == 0) {
                do_clear = 1;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -clear)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-clear?");
            code = TCL_ERROR;
            goto done;
        }
        if (do_clear) {
            Tcl_MutexLock(&ctx->mutex);
            MsgQ_Clear(&ctx->msgq);
            Tcl_MutexUnlock(&ctx->mutex);
            code = TCL_OK;
            goto done;
        } else {
            Tcl_Obj *list = Tcl_NewListObj(0, NULL);
            Tcl_MutexLock(&ctx->mutex);
            for (size_t i = 0; i < ctx->msgq.count; i++) {
                Tcl_ListObjAppendElement(interp, list, Tcl_NewStringObj(ctx->msgq.items[i], -1));
            }
            Tcl_MutexUnlock(&ctx->mutex);
            Tcl_SetObjResult(interp, list);
            code = TCL_OK;
            goto done;
        }
    }
    if (strcmp(sub, "eventcounts") == 0) {
        int do_clear = 0;
        if (objc == 3) {
            const char *opt = Tcl_GetString(objv[2]);
            if (strcmp(opt, "-clear") == 0) {
                do_clear = 1;
            } else {
                Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s (expected -clear)", opt));
                code = TCL_ERROR;
                goto done;
            }
        } else if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "?-clear?");
            code = TCL_ERROR;
            goto done;
        }
        Tcl_MutexLock(&ctx->mutex);
        if (do_clear) {
            memset(ctx->evt_counts, 0, sizeof(ctx->evt_counts));
            Tcl_MutexUnlock(&ctx->mutex);
            code = TCL_OK;
            goto done;
        }
        uint64_t c[NUM_EVTS];
        memcpy(c, ctx->evt_counts, sizeof(c));
        Tcl_MutexUnlock(&ctx->mutex);
        Tcl_Obj *d = Tcl_NewDictObj();
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("send_char", -1), Tcl_NewWideIntObj((Tcl_WideInt)c[SEND_CHAR]));
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("send_stat", -1), Tcl_NewWideIntObj((Tcl_WideInt)c[SEND_STAT]));
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("controlled_exit", -1),
                       Tcl_NewWideIntObj((Tcl_WideInt)c[CONTROLLED_EXIT]));
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("send_data", -1), Tcl_NewWideIntObj((Tcl_WideInt)c[SEND_DATA]));
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("send_init_data", -1),
                       Tcl_NewWideIntObj((Tcl_WideInt)c[SEND_INIT_DATA]));
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("bg_running", -1),
                       Tcl_NewWideIntObj((Tcl_WideInt)c[BG_THREAD_RUNNING]));
        Tcl_SetObjResult(interp, d);
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "destroy") == 0) {
        Tcl_Command token = Tcl_GetCommandFromObj(interp, objv[0]);
        Tcl_DeleteCommandFromToken(interp, token);
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "abort") == 0) {
        ctx->aborting = 1;
        Tcl_MutexLock(&ctx->mutex);
        Tcl_ConditionNotify(&ctx->cond);
        Tcl_MutexUnlock(&ctx->mutex);
        Tcl_SetObjResult(interp, Tcl_NewStringObj("aborted", -1));
        code = TCL_OK;
        goto done;
    }
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown subcommand: %s", sub));
    code = TCL_ERROR;
    goto done;
done:
    Tcl_Release((ClientData)ctx);
    return code;
}
//***  NgSpiceNewCmd function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * RESOLVE_OR_BAIL (macro) --
 *
 *      Helper macro used during NgSpiceNewCmd initialization to resolve a required symbol from the loaded ngspice
 *      shared library. If the symbol cannot be found, the macro immediately closes the library, frees the allocated
 *      NgSpiceContext, and returns TCL_ERROR from the enclosing function.
 *
 * Parameters:
 *      field    - struct field in NgSpiceContext to store the resolved function pointer
 *      symname  - string name of the symbol to resolve
 *
 * Results:
 *      - If successful, assigns the resolved function pointer to ctx->field.
 *      - If unsuccessful, performs cleanup and bails out of the caller with TCL_ERROR.
 *
 * Side Effects:
 *      - On failure, releases the open library handle and frees the NgSpiceContext structure
 *      - Terminates execution of the calling function via return
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * NgSpiceNewCmd --
 *
 *      Implements the ::ngspicetclbridge::new command. Creates a new Tcl command instance that wraps an ngspice
 *      simulation context. Dynamically loads the specified ngspice shared library, resolves all required API symbols,
 *      and initializes internal synchronization structures and buffers.
 *
 *      A unique command name is generated for each instance in the format:
 *          ::ngspicetclbridge::s<N>
 *      where N is a monotonically increasing sequence number.
 *
 * Parameters:
 *      ClientData cd         - input: not used (reserved for future use)
 *      Tcl_Interp *interp    - input: target interpreter in which to create the new command
 *      Tcl_Size objc         - input: number of arguments
 *      Tcl_Obj *const objv[] - input: argument vector; expects exactly 2 arguments:
 *                                  objv[0] = command name ("::ngspicetclbridge::new")
 *                                  objv[1] = path to ngspice shared library (DLL/.so/.dylib)
 *
 * Results:
 *      On success:
 *          - Returns TCL_OK
 *          - Sets the interpreter result to the unique instance command name
 *      On failure:
 *          - Returns TCL_ERROR
 *          - Sets the interpreter result to an error message
 *
 * Side Effects:
 *      - Calls ngSpice_Init() with our callbacks:
 *          SendCharCallback, SendStatCallback, ControlledExitCallback,
 *          SendDataCallback, SendInitDataCallback, BGThreadRunningCallback.
 *      - Allocates and initializes an NgSpiceContext structure
 *      - Opens the specified shared library and resolves all required symbols
 *      - Registers a new instance-specific Tcl object command bound to InstObjCmd()
 *      - On any error during symbol resolution, closes the library, frees the context, and aborts creation
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
#define RESOLVE_OR_BAIL(field, symname)                                                                                \
    do {                                                                                                               \
        ctx->field = (typeof(ctx->field))PDl_Sym(interp, ctx->handle, symname);                                        \
        if (!(ctx->field)) {                                                                                           \
            PDl_Close(ctx->handle);                                                                                    \
            Tcl_Free(ctx);                                                                                             \
            return TCL_ERROR;                                                                                          \
        }                                                                                                              \
    } while (0)

static int NgSpiceNewCmd(ClientData cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    Tcl_Obj *libPathObj;
    int selector = 0;
    if (objc == 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "libpath");
        return TCL_ERROR;
    } else if (objc == 2) {
        const char *opt = Tcl_GetString(objv[1]);
        if ((strcmp(opt, "-nospinit") == 0) || (strcmp(opt, "-nospiceinit") == 0) || (strcmp(opt, "-noinit") == 0)) {
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("in case of one argument, it must be library path, not %s", opt));
            return TCL_ERROR;
        }
        libPathObj = objv[1];
    } else if (objc == 3) {
        const char *opt = Tcl_GetString(objv[1]);
        libPathObj = objv[2];
        if (strcmp(opt, "-nospinit") == 0) {
            selector = 1;
        } else if (strcmp(opt, "-nospiceinit") == 0) {
            selector = 2;
        } else if (strcmp(opt, "-noinit") == 0) {
            selector = 3;
        } else {
            Tcl_SetObjResult(interp,
                             Tcl_ObjPrintf("unknown option: %s (expected -nospinit, -nospiceinit or -noinit)", opt));
            return TCL_ERROR;
        }
    } else {
        Tcl_WrongNumArgs(interp, 1, objv, "-nospinit|-nospiceinit|-noinit libpath");
        return TCL_ERROR;
    }
    
    NgSpiceContext *ctx = (NgSpiceContext *)Tcl_Alloc(sizeof *ctx);
    memset(ctx, 0, sizeof *ctx);
    ctx->exited = 0;
    MsgQ_Init(&ctx->capq);
    MsgQ_Init(&ctx->msgq);
    DataBuf_Init(&ctx->prod);
    DataBuf_Init(&ctx->pend);
    ctx->interp = interp;
    ctx->tclid = Tcl_GetCurrentThread();
    memset(ctx->evt_counts, 0, sizeof(ctx->evt_counts));
    ctx->handle = PDl_OpenFromObj(interp, libPathObj);
    if (!ctx->handle) {
        Tcl_Free(ctx);
        return TCL_ERROR;
    }
    RESOLVE_OR_BAIL(ngSpice_Init, "ngSpice_Init");
    RESOLVE_OR_BAIL(ngSpice_Init_Sync, "ngSpice_Init_Sync");
    RESOLVE_OR_BAIL(ngSpice_Command, "ngSpice_Command");
    RESOLVE_OR_BAIL(ngGet_Vec_Info, "ngGet_Vec_Info");
    RESOLVE_OR_BAIL(ngCM_Input_Path, "ngCM_Input_Path");
    RESOLVE_OR_BAIL(ngGet_Evt_NodeInfo, "ngGet_Evt_NodeInfo");
    RESOLVE_OR_BAIL(ngSpice_AllEvtNodes, "ngSpice_AllEvtNodes");
    RESOLVE_OR_BAIL(ngSpice_Init_Evt, "ngSpice_Init_Evt");
    RESOLVE_OR_BAIL(ngSpice_Circ, "ngSpice_Circ");
    RESOLVE_OR_BAIL(ngSpice_CurPlot, "ngSpice_CurPlot");
    RESOLVE_OR_BAIL(ngSpice_AllPlots, "ngSpice_AllPlots");
    RESOLVE_OR_BAIL(ngSpice_AllVecs, "ngSpice_AllVecs");
    RESOLVE_OR_BAIL(ngSpice_running, "ngSpice_running");
    RESOLVE_OR_BAIL(ngSpice_SetBkpt, "ngSpice_SetBkpt");
    RESOLVE_OR_BAIL(ngSpice_nospinit, "ngSpice_nospinit");
    RESOLVE_OR_BAIL(ngSpice_nospiceinit, "ngSpice_nospiceinit");
    RESOLVE_OR_BAIL(ngSpice_LockRealloc, "ngSpice_LockRealloc");
    RESOLVE_OR_BAIL(ngSpice_UnlockRealloc, "ngSpice_UnlockRealloc");
    static unsigned long seq = 0;
    Tcl_Obj *name = Tcl_ObjPrintf("::ngspicetclbridge::s%lu", ++seq);
    Tcl_CreateObjCommand2(interp, Tcl_GetString(name), InstObjCmd, ctx, InstDeleteProc);
    ctx->ngSpice_Init(SendCharCallback, SendStatCallback, ControlledExitCallback, SendDataCallback,
                      SendInitDataCallback, BGThreadRunningCallback, ctx);
    if (!ctx->vectorData) {
        ctx->vectorData = Tcl_NewDictObj();
        Tcl_IncrRefCount(ctx->vectorData);
    }
    if (!ctx->vectorInit) {
        ctx->vectorInit = Tcl_NewDictObj();
        Tcl_IncrRefCount(ctx->vectorInit);
    }
    if (selector == 1) {
        ctx->ngSpice_nospinit();
    } else if (selector == 2) {
        ctx->ngSpice_nospiceinit();
    } else if (selector == 3) {
        ctx->ngSpice_nospinit();
        ctx->ngSpice_nospiceinit();
    }
    Tcl_SetObjResult(interp, name);
    return TCL_OK;
}
#undef RESOLVE_OR_BAIL
//***  Ngspicetclbridge_Init function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * Ngspicetclbridge_Init --
 *
 *      Package initialization function for the ngspicetclbridge extension.
 *      Registers the "::ngspicetclbridge::new" command, creates the extension namespace,
 *      and declares the package to the Tcl interpreter.
 *
 * Parameters:
 *      Tcl_Interp *interp             - input: target interpreter where the package is being loaded
 *
 * Results:
 *      Returns TCL_OK on successful initialization, TCL_ERROR if initialization fails.
 *      On failure, leaves an appropriate error message in the interpreter result.
 *
 * Side Effects:
 *      - Initializes Tcl stubs for compatibility with Tcl versions 8.6 through 10.0.
 *      - Creates the namespace "::ngspicetclbridge" if it does not already exist.
 *      - Registers the "::ngspicetclbridge::new" command to create new ngspice bridge instances.
 *      - Marks the package as provided via Tcl_PkgProvideEx() using PACKAGE_NAME and PACKAGE_VERSION.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
DLLEXPORT int Ngspicetclbridge_Init(Tcl_Interp *interp) {
    if (Tcl_InitStubs(interp, "8.6-10.0", 0) == NULL) {
        return TCL_ERROR;
    }
    if (Tcl_Eval(interp, "namespace eval ::ngspicetclbridge:: {}") != TCL_OK) {
        return TCL_ERROR;
    }
    Tcl_CreateObjCommand2(interp, "::ngspicetclbridge::new", NgSpiceNewCmd, NULL, NULL);
    if (Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION, NULL) != TCL_OK) {
        return TCL_ERROR;
    }
    return TCL_OK;
}
