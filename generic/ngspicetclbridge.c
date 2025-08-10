#include "ngspicetclbridge.h"

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
    ctx->evt_counts[which]++;        // monotonic
    Tcl_ConditionNotify(&ctx->cond); // wake a waiter
    Tcl_MutexUnlock(&ctx->mutex);
}
//***  wait_for function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * wait_for --
 *
 *      Wait for a specific event counter in an NgSpiceContext to increment, with optional timeout and abort handling.
 *      This function blocks until one of the following occurs:
 *          - The specified event counter changes (indicating an event has fired)
 *          - The context is marked for destruction (abort condition)
 *          - The timeout period expires
 *
 * Parameters:
 *      NgSpiceContext *ctx          - input/output: pointer to the ngspice context containing the mutex,
 *                                      event counters, and condition variable
 *      int which                    - input: index into ctx->evt_counts[] identifying the event to monitor
 *      long timeout_ms              - input: timeout in milliseconds; 0 or negative means wait indefinitely
 *      int *fired_out               - output (optional): set to nonzero if the event fired during the wait
 *      uint64_t *count_out          - output (optional): set to the current total count for this event when the
 *                                      wait terminates
 *
 * Results:
 *      Returns one of:
 *          WAIT_OK       - event fired during the wait
 *          WAIT_TIMEOUT  - timeout expired before event fired
 *          WAIT_ABORTED  - context was marked for destruction before event fired
 *
 * Side Effects:
 *      Locks and unlocks ctx->mutex around counter checks and condition waits.
 *      May sleep in slices (25 ms by default) for finite timeouts, polling the counter between sleeps.
 *      For indefinite waits, uses Tcl_ConditionWait() to block until signaled or aborted.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static wait_rc wait_for(NgSpiceContext *ctx, int which, long timeout_ms, int *fired_out, uint64_t *count_out) {
    const int slice_ms = 25;
    long remaining = timeout_ms;
    Tcl_MutexLock(&ctx->mutex);
    uint64_t start = ctx->evt_counts[which];
    /* fast path */
    if (ctx->evt_counts[which] > start) {
        uint64_t cnt = ctx->evt_counts[which];
        Tcl_MutexUnlock(&ctx->mutex);
        if (fired_out) {
            *fired_out = 1;
        }
        if (count_out) {
            *count_out = cnt;
        }
        return WAIT_OK;
    }
    if (timeout_ms <= 0) {
        while (!ctx->destroying && ctx->evt_counts[which] == start) {
            Tcl_ConditionWait(&ctx->cond, &ctx->mutex, NULL);
        }
    } else {
        while (!ctx->destroying && ctx->evt_counts[which] == start && remaining > 0) {
            Tcl_MutexUnlock(&ctx->mutex);
            int step = (remaining < slice_ms) ? (int)remaining : slice_ms;
            Tcl_Sleep(step);
            remaining -= step;
            Tcl_MutexLock(&ctx->mutex);
        }
    }
    int fired = (ctx->evt_counts[which] > start);
    uint64_t cnt = ctx->evt_counts[which];
    int aborted = ctx->destroying;
    Tcl_MutexUnlock(&ctx->mutex);
    if (fired_out) {
        *fired_out = fired;
    }
    if (count_out) {
        *count_out = cnt;
    }
    if (aborted) {
        return WAIT_ABORTED;
    }
    if (!fired && timeout_ms > 0) {
        return WAIT_TIMEOUT;
    }
    return WAIT_OK;
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
    if (!r)
        return;
    for (int i = 0; i < r->veccount; i++)
        Tcl_Free(r->vecs[i].name);
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
    for (size_t i = 0; i < b->count; i++)
        FreeDataRow(&b->rows[i]);
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
    if (need <= b->cap)
        return;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    if (ncap < need)
        ncap = need;
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
 *      and performs any required Tcl_Obj manipulations in a thread-safe manner.
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
            /* free snapshot */
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
        /* other events don’t need per-event work here */
        break;
    }
    Tcl_Release((ClientData)ctx);
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
static void NgSpiceQueueEvent(NgSpiceContext *ctx, int callbackId) {
    if (ctx->destroying) {
        return;
    }
    Tcl_Preserve((ClientData)ctx);
    NgSpiceEvent *ev = (NgSpiceEvent *)Tcl_Alloc(sizeof *ev);
    ev->header.proc = NgSpiceEventProc;
    ev->header.nextPtr = NULL;
    ev->ctx = ctx;
    ev->callbackId = callbackId;
    if (Tcl_GetCurrentThread() == ctx->tclid) {
        Tcl_QueueEvent((Tcl_Event *)ev, TCL_QUEUE_TAIL);
    } else {
        Tcl_ThreadQueueEvent(ctx->tclid, (Tcl_Event *)ev, TCL_QUEUE_TAIL);
        Tcl_ThreadAlert(ctx->tclid);
    }
}
//***  MatchNgSpiceEvent function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * MatchNgSpiceEvent --
 *
 *      Predicate function for identifying NgSpiceEvent structures in the Tcl event queue
 *      that belong to a specific NgSpiceContext.
 *
 * Parameters:
 *      Tcl_Event *evPtr             - input: pointer to a queued Tcl_Event, expected to be an NgSpiceEvent
 *      ClientData cd                - input: pointer to the NgSpiceContext to match against
 *
 * Results:
 *      Returns nonzero (true) if:
 *          - evPtr->proc is NgSpiceEventProc, and
 *          - evPtr->ctx matches the NgSpiceContext pointer in cd
 *      Otherwise returns 0 (false).
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int MatchNgSpiceEvent(Tcl_Event *evPtr, ClientData cd) {
    NgSpiceEvent *e = (NgSpiceEvent *)evPtr;
    return (e->header.proc == NgSpiceEventProc && e->ctx == (NgSpiceContext *)cd);
}
//***  QuiesceNgspice function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * QuiesceNgspice --
 *
 *      Request that the ngspice background thread stop execution, and optionally poll for confirmation.
 *      Intended to bring ngspice into an idle state before tearing down the NgSpiceContext or releasing
 *      related resources.
 *
 * Parameters:
 *      NgSpiceContext *ctx          - input/output: pointer to the ngspice context; may be NULL
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - If ctx is NULL or ctx->ngSpice_Command is NULL, returns immediately without action.
 *      - Invokes ctx->ngSpice_Command("bg_halt") to request the background thread stop
 *      - If ctx->ngSpice_running is available, polls it up to ~2 seconds (40 × 50 ms) until it
 *        reports 0 (not running), sleeping 50 ms between checks.
 *      - If ctx->ngSpice_running is not available, sleeps 200 ms as a fallback delay.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void QuiesceNgspice(NgSpiceContext *ctx) {
    if (!ctx || !ctx->ngSpice_Command) {
        return;
    }
    // Ask the bg thread to stop; don't wait here
    ctx->ngSpice_Command("bg_halt");
    // Poll ngSpice_running() briefly (if available). 0 == not running.
    if (ctx->ngSpice_running) {
        for (int i = 0; i < 40; i++) {     // ~2s max
            if (ctx->ngSpice_running() == 0) break;
            Tcl_Sleep(50);
        }
    } else {
        // Fallback: just give it a moment
        Tcl_Sleep(200);
    }
}
//** ngspice callbacks (instance-scoped via ctx user ptr)
//***  SendCharCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendCharCallback --
 *
 *      ngspice callback function invoked whenever a new message is produced on ngspice's stdout or stderr streams.
 *      Captures the message, updates event counters, and queues a corresponding Tcl event for processing
 *      in the main thread.
 *
 * Parameters:
 *      char *msg                    - input: pointer to the NUL-terminated message string from ngspice
 *      int id                       - input: identification number of the calling ngspice shared library
 *                                      instance (default is 0; may be ignored)
 *      void *user                   - input: user data pointer supplied during ngspice initialization,
 *                                      expected to be an NgSpiceContext *
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid, msg is non-NULL, and ctx->destroying is false:
 *          - Appends a copy of msg to ctx->msgq via QueueMsg().
 *          - Increments the SEND_CHAR event counter and signals waiters via BumpAndSignal().
 *          - Queues a SEND_CHAR Tcl event via NgSpiceQueueEvent() for deferred processing.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int SendCharCallback(char *msg, int id, void *user) {
    /* ngspice make callback each time new message appears in stdout or stderr */
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx || !msg || ctx->destroying) {
        return 0;
    }
    QueueMsg(ctx, msg);
    BumpAndSignal(ctx, SEND_CHAR);
    NgSpiceQueueEvent(ctx, SEND_CHAR);
    return 0;
}
//***  SendStatCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendStatCallback --
 *
 *      ngspice callback function invoked whenever the simulation status changes.
 *      Formats the status message, updates event counters, and queues a corresponding
 *      Tcl event for processing in the main thread.
 *
 * Parameters:
 *      char *msg                    - input: pointer to the NUL-terminated status message from ngspice
 *      int id                       - input: identification number of the calling ngspice shared library
 *                                      instance (default is 0)
 *      void *user                   - input: user data pointer supplied during ngspice initialization,
 *                                      expected to be an NgSpiceContext *
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid and ctx->destroying is false:
 *          - Formats the status line as "# status[<id>]: <msg>" into a fixed-size buffer (128 bytes + terminator).
 *          - Appends the formatted line to ctx->msgq via QueueMsg().
 *          - Increments the SEND_STAT event counter and signals waiters via BumpAndSignal().
 *          - Queues a SEND_STAT Tcl event via NgSpiceQueueEvent() for deferred processing.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int SendStatCallback(char *msg, int id, void *user) {
    /* ngspice make callback each time status of simulation changed */
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx || ctx->destroying) {
        return 0;
    }
    char line[128 + 1]; // small status line
    snprintf(line, sizeof line, "# status[%d]: %s", id, msg);
    QueueMsg(ctx, line);
    BumpAndSignal(ctx, SEND_STAT);
    NgSpiceQueueEvent(ctx, SEND_STAT);
    return 0;
}
//***  ControlledExitCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * ControlledExitCallback --
 *
 *      ngspice callback function invoked when ngspice exits, either due to an error or as the
 *      result of a quit command. Formats the exit information, updates event counters, and
 *      queues a corresponding Tcl event for processing in the main thread.
 *
 * Parameters:
 *      int status                   - input: exit status code from ngspice
 *      bool immediate               - input: nonzero if ngspice exited immediately without cleanup
 *      bool exit_upon_exit           - input: nonzero if exit was initiated by user request (quit command)
 *      int id                        - input: identification number of the calling ngspice shared library
 *                                      instance (default is 0)
 *      void *user                    - input: user data pointer supplied during ngspice initialization,
 *                                      expected to be an NgSpiceContext *
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid and ctx->destroying is false:
 *          - Formats an exit message string into the form:
 *                "# ngspice exited: status=<status> immediate=<immediate> userquit=<exit_upon_exit>"
 *          - Appends the formatted message to ctx->msgq via QueueMsg().
 *          - Increments the CONTROLLED_EXIT event counter and signals waiters via BumpAndSignal().
 *          - Queues a CONTROLLED_EXIT Tcl event via NgSpiceQueueEvent() for deferred processing.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
int ControlledExitCallback(int status, bool immediate, bool exit_upon_exit, int id, void *user) {
    /* ngspice make callback when ngspice is exited, because of error or quit command */
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx || ctx->destroying) {
        return 0;
    }
    char line[160];
    snprintf(line, sizeof line, "# ngspice exited: status=%d immediate=%d userquit=%d", status, immediate,
             exit_upon_exit);
    QueueMsg(ctx, line);
    BumpAndSignal(ctx, CONTROLLED_EXIT);
    NgSpiceQueueEvent(ctx, CONTROLLED_EXIT);
    return 0;
}
//***  SendDataCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendDataCallback --
 *
 *      ngspice callback function invoked whenever simulation data values are produced.
 *      Captures the data into a newly allocated DataRow, appends it to the producer buffer,
 *      updates event counters, and queues a corresponding Tcl event for processing
 *      in the main thread.
 *
 * Parameters:
 *      pvecvaluesall all            - input: pointer to structure containing all vector values for this timepoint
 *      int count                    - input: number of simulation points in this callback (unused if > 0 check passes)
 *      int id                       - input: identification number of the calling ngspice shared library instance
 *      void *user                   - input: user data pointer supplied during ngspice initialization,
 *                                      expected to be an NgSpiceContext *
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid, all is non-NULL, count > 0, and ctx->destroying is false:
 *          - Allocates a DataRow with veccount = all->veccount.
 *          - Allocates row.vecs[] and duplicates each vector name with ckstrdup().
 *          - Copies is_complex, creal, and cimag values for each vector from all->vecsa[].
 *          - Locks ctx->mutex, ensures ctx->prod has room for one more DataRow via DataBuf_Ensure(),
 *            and appends the new row.
 *          - Unlocks ctx->mutex.
 *          - Increments the SEND_DATA event counter and signals waiters via BumpAndSignal().
 *          - Queues a SEND_DATA Tcl event via NgSpiceQueueEvent() for deferred processing.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int SendDataCallback(pvecvaluesall all, int count, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
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
    DataBuf_Ensure(&ctx->prod, ctx->prod.count + 1);
    ctx->prod.rows[ctx->prod.count++] = row;
    Tcl_MutexUnlock(&ctx->mutex);
    BumpAndSignal(ctx, SEND_DATA);
    NgSpiceQueueEvent(ctx, SEND_DATA);
    return 0;
}
//***  SendInitDataCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * SendInitDataCallback --
 *
 *      ngspice callback function invoked when initial simulation vector metadata is available.
 *      Captures vector names, numbers, and type flags into a newly allocated InitSnap structure,
 *      replaces any existing snapshot in the NgSpiceContext, updates event counters, and queues
 *      a corresponding Tcl event for processing in the main thread.
 *
 * Parameters:
 *      pvecinfoall vinfo             - input: pointer to structure containing vector metadata
 *      int id                        - input: identification number of the calling ngspice shared library instance
 *      void *user                    - input: user data pointer supplied during ngspice initialization,
 *                                       expected to be an NgSpiceContext *
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid, vinfo is non-NULL, and ctx->destroying is false:
 *          - Allocates an InitSnap structure sized for vinfo->veccount vectors.
 *          - For each vector in vinfo->vecs:
 *              - Duplicates the vector name with ckstrdup().
 *              - Copies the numeric identifier and is_real flag.
 *          - Locks ctx->mutex, frees any existing ctx->init_snap (including its name strings and array),
 *            and replaces it with the new InitSnap.
 *          - Unlocks ctx->mutex.
 *          - Increments the SEND_INIT_DATA event counter and signals waiters via BumpAndSignal().
 *          - Queues a SEND_INIT_DATA Tcl event via NgSpiceQueueEvent() for deferred processing.
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
    Tcl_MutexUnlock(&ctx->mutex);
    BumpAndSignal(ctx, SEND_INIT_DATA);
    NgSpiceQueueEvent(ctx, SEND_INIT_DATA);
    return 0;
}
//***  BGThreadRunningCallback function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * BGThreadRunningCallback --
 *
 *      ngspice callback function invoked when the state of the background simulation thread changes.
 *      Logs a message describing whether the background thread has started or ended, updates event
 *      counters, and queues a corresponding Tcl event for processing in the main thread.
 *
 * Parameters:
 *      bool running                  - input: false if the background thread is running, true if it has stopped
 *      int id                        - input: identification number of the calling ngspice shared library instance
 *      void *user                    - input: user data pointer supplied during ngspice initialization,
 *                                        expected to be an NgSpiceContext *
 *
 * Results:
 *      Always returns 0 (ignored by ngspice).
 *
 * Side Effects:
 *      - If ctx is valid and ctx->destroying is false:
 *          - Appends a status message to ctx->msgq via QueueMsg():
 *              "# background thread running started" if running == false
 *              "# background thread running ended"   if running == true
 *          - Increments the BG_THREAD_RUNNING event counter and signals waiters via BumpAndSignal().
 *          - Queues a BG_THREAD_RUNNING Tcl event via NgSpiceQueueEvent() for deferred processing.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static int BGThreadRunningCallback(bool running, int id, void *user) {
    NgSpiceContext *ctx = (NgSpiceContext *)user;
    if (!ctx || ctx->destroying) {
        return 0;
    }
    QueueMsg(ctx, running ? "# background thread running ended" : "# background thread running started");
    BumpAndSignal(ctx, BG_THREAD_RUNNING);
    NgSpiceQueueEvent(ctx, BG_THREAD_RUNNING);
    return 0;
}
//** free functions
//***  InstFreeProc function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * InstFreeProc --
 *
 *      Instance finalizer for an NgSpiceContext. Cleans up all resources, stops any background
 *      ngspice activity, and frees all dynamically allocated memory associated with the context.
 *
 * Parameters:
 *      void *cdata                   - input: pointer to the NgSpiceContext to destroy
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Calls QuiesceNgspice() to request that ngspice stop its background thread.
 *      - Removes any queued NgSpiceEvent entries associated with this context via Tcl_DeleteEvents().
 *      - Signals ctx->cond to wake any threads waiting in wait_for(), ensuring no thread remains blocked.
 *      - Decrements the reference counts of ctx->vectorData and ctx->vectorInit if they are set.
 *      - Frees all queued messages with MsgQ_Free().
 *      - Frees the producer and pending DataBuf buffers with DataBuf_Free().
 *      - Finalizes the condition variable and mutex.
 *      - Closes the dynamically loaded ngspice library handle if present via dlclose().
 *      - Frees the NgSpiceContext structure itself with Tcl_Free().
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void InstFreeProc(void *cdata) {
    NgSpiceContext *ctx = (NgSpiceContext *)cdata;
    QuiesceNgspice(ctx);
    Tcl_DeleteEvents(MatchNgSpiceEvent, ctx);
    Tcl_MutexLock(&ctx->mutex);
    Tcl_ConditionNotify(&ctx->cond); // belt-and-braces wake
    Tcl_MutexUnlock(&ctx->mutex);
    if (ctx->vectorData) {
        Tcl_DecrRefCount(ctx->vectorData);
    }
    if (ctx->vectorInit) {
        Tcl_DecrRefCount(ctx->vectorInit);
    }
    MsgQ_Free(&ctx->msgq);
    DataBuf_Free(&ctx->prod);
    DataBuf_Free(&ctx->pend);
    Tcl_ConditionFinalize(&ctx->cond);
    Tcl_MutexFinalize(&ctx->mutex);
    if (ctx->handle) {
        PDl_Close(ctx->handle);
    }
    Tcl_Free(ctx);
}
//***  InstDeleteProc function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * InstDeleteProc --
 *
 *      Instance deletion handler for an NgSpiceContext. Marks the context as being destroyed,
 *      wakes any threads waiting on its condition variable, and schedules deferred cleanup
 *      via InstFreeProc() once it is safe to free the context.
 *
 * Parameters:
 *      void *cdata                   - input: pointer to the NgSpiceContext to delete
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      - Sets ctx->destroying to 1, signaling other threads and callbacks to stop using the context.
 *      - Acquires ctx->mutex, notifies ctx->cond to wake any blocked waiters, and releases the mutex.
 *      - Calls Tcl_EventuallyFree() to schedule InstFreeProc() for execution when no other code
 *        holds a reference to ctx.
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
static void InstDeleteProc(void *cdata) {
    NgSpiceContext *ctx = (NgSpiceContext *)cdata;
    ctx->destroying = 1;
    Tcl_MutexLock(&ctx->mutex);
    Tcl_ConditionNotify(&ctx->cond); // wake any waiters
    Tcl_MutexUnlock(&ctx->mutex);
    Tcl_EventuallyFree((ClientData)ctx, InstFreeProc);
}
//** command registering function
//***  InstObjCmd function
/*
 *----------------------------------------------------------------------------------------------------------------------
 *
 * InstObjCmd --
 *
 *      Object command procedure for an NgSpiceContext instance. Dispatches subcommands to control,
 *      query, or modify the state of the ngspice interface and associated data structures.
 *
 * Parameters:
 *      ClientData cdata              - input: pointer to the NgSpiceContext for this command instance
 *      Tcl_Interp *interp            - input: target interpreter
 *      Tcl_Size objc                 - input: number of arguments in objv[]
 *      Tcl_Obj *const objv[]         - input: argument list, where objv[0] is the instance command name
 *                                       and objv[1] is the subcommand
 *
 * Results:
 *      Returns a standard Tcl result code (TCL_OK or TCL_ERROR).
 *      Sets the interpreter result for subcommands that produce output.
 *
 * Supported Subcommands:
 *      init
 *          - Calls ngSpice_Init() with registered callbacks and ensures vectorData/vectorInit dicts exist.
 *          - Returns ngspice initialization result code (int).
 *
 *      command string
 *          - Sends an arbitrary ngspice command string via ngSpice_Command().
 *          - Returns the ngspice result code (int).
 *
 *      waitevent name ?timeout_ms?
 *          - Waits for a named ngspice event (e.g., "send_stat") up to the specified timeout in milliseconds.
 *          - Returns a dict: {fired <bool> count <int64> status <ok|timeout|aborted>}.
 *
 *      vectors ?-clear?
 *          - Without -clear: returns ctx->vectorData (dict of simulation vector values).
 *          - With -clear: replaces vectorData with an empty dict (no result returned).
 *
 *      asyncvector string
 *          - Returns list with data of provided vector by its name.
 *
 *      initvectors ?-clear?
 *          - Without -clear: returns ctx->vectorInit (dict of vector metadata).
 *          - With -clear: replaces vectorInit with an empty dict (no result returned).
 *
 *      messages ?-clear?
 *          - Without -clear: returns a list of queued messages from ngspice.
 *          - With -clear: clears the message queue (no result returned).
 *
 *      eventcounts ?-clear?
 *          - Without -clear: returns a dict mapping event names to their counts.
 *          - With -clear: zeros all event counters (no result returned).
 *
 *      isrunning
 *          - Returns 1 if background thread is running, 0 otherwise.
 *
 *      destroy
 *          - Deletes the instance command, triggering InstDeleteProc().
 *
 *      abort
 *          - Sets ctx->destroying to true and wakes any condition waiters, without freeing resources.
 *
 * Side Effects:
 *      - Many subcommands modify NgSpiceContext state (e.g., clear/reset data, increment event counters).
 *      - Calls into ngspice shared library functions (Init, Command).
 *      - Manages Tcl reference counts for shared Tcl_Obj structures.
 *      - Uses Tcl_Mutex/Tcl_Condition to protect and signal state changes.
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
    if (strcmp(sub, "init") == 0) {
        int rc = ctx->ngSpice_Init(SendCharCallback, SendStatCallback, ControlledExitCallback, SendDataCallback,
                                   SendInitDataCallback, BGThreadRunningCallback, ctx);
        if (!ctx->vectorData) {
            ctx->vectorData = Tcl_NewDictObj();
            Tcl_IncrRefCount(ctx->vectorData);
        }
        if (!ctx->vectorInit) {
            ctx->vectorInit = Tcl_NewDictObj();
            Tcl_IncrRefCount(ctx->vectorInit);
        }
        Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "command") == 0) {
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "string");
            code = TCL_ERROR;
            goto done;
        }
        const char *cmd = Tcl_GetString(objv[2]);
        int rc = ctx->ngSpice_Command((char *)cmd);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "waitevent") == 0) {
        if (objc < 3 || objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "name ?timeout_ms?");
            code = TCL_ERROR;
            goto done;
        }
        int which;
        if (NameToEvtId(objv[2], &which) != TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown event: %s", Tcl_GetString(objv[2])));
            code = TCL_ERROR;
            goto done;
        }
        long to = 0;
        if (objc == 4 && Tcl_GetLongFromObj(interp, objv[3], &to) != TCL_OK) {
            code = TCL_ERROR;
            goto done;
        }
        int fired;
        int status;
        uint64_t count;
        status = wait_for(ctx, which, to, &fired, &count);
        Tcl_Obj *res = Tcl_NewDictObj();
        Tcl_DictObjPut(interp, res, Tcl_NewStringObj("fired", -1), Tcl_NewBooleanObj(fired));
        Tcl_DictObjPut(interp, res, Tcl_NewStringObj("count", -1), Tcl_NewWideIntObj((Tcl_WideInt)count));
        if (status == WAIT_TIMEOUT) {
            Tcl_DictObjPut(interp, res, Tcl_NewStringObj("status", -1), Tcl_NewStringObj("timeout", -1));
        } else if (status == WAIT_ABORTED) {
            Tcl_DictObjPut(interp, res, Tcl_NewStringObj("status", -1), Tcl_NewStringObj("aborted", -1));
        } else if (status == WAIT_OK) {
            Tcl_DictObjPut(interp, res, Tcl_NewStringObj("status", -1), Tcl_NewStringObj("ok", -1));
        }
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
    if (strcmp(sub, "asyncvector") == 0) {
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "string");
            code = TCL_ERROR;
            goto done;
        }
        const char *vecname = Tcl_GetString(objv[2]);
        pvector_info vinfo = ctx->ngGet_Vec_Info((char *)vecname);
        if (vinfo == NULL) {
            Tcl_Obj *errMsg = Tcl_ObjPrintf("vector with name \"%s\" does not exist", vecname);
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
        Tcl_SetObjResult(interp, dataObj);
        code = TCL_OK;
        goto done;
    }
    if (strcmp(sub, "isrunning") == 0) {
        if (objc > 2) {
            Tcl_WrongNumArgs(interp, 1, objv, NULL);
            code = TCL_ERROR;
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
            // No result returned on clear
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
        Tcl_DictObjPut(interp, d, Tcl_NewStringObj("bg_running", -1), Tcl_NewWideIntObj((Tcl_WideInt)c[BG_THREAD_RUNNING]));
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
        // Synchronously request abort; no event processing, no freeing here.
        ctx->destroying = 1;
        Tcl_MutexLock(&ctx->mutex);
        Tcl_ConditionNotify(&ctx->cond);
        Tcl_MutexUnlock(&ctx->mutex);
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
 *      - Allocates and initializes an NgSpiceContext structure
 *      - Opens the specified shared library and resolves all required symbols
 *      - Registers a new instance-specific Tcl object command bound to InstObjCmd()
 *      - On any error during symbol resolution, closes the library, frees the context, and aborts creation
 *
 *----------------------------------------------------------------------------------------------------------------------
 */
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
#define RESOLVE_OR_BAIL(field, symname)                                                                                \
    do {                                                                                                               \
        ctx->field = (typeof(ctx->field))PDl_Sym(interp, ctx->handle, symname);                                            \
        if (!(ctx->field)) {                                                                                           \
            PDl_Close(ctx->handle);                                                                                    \
            Tcl_Free(ctx);                                                                                             \
            return TCL_ERROR;                                                                                          \
        }                                                                                                              \
    } while (0)

static int NgSpiceNewCmd(ClientData cd, Tcl_Interp *interp, Tcl_Size objc, Tcl_Obj *const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "library_path");
        return TCL_ERROR;
    }
    Tcl_Obj *libPathObj = objv[1];
    NgSpiceContext *ctx = (NgSpiceContext *)Tcl_Alloc(sizeof *ctx);
    memset(ctx, 0, sizeof *ctx);
    /* init mutex/cond and local state */
    MsgQ_Init(&ctx->msgq);
    DataBuf_Init(&ctx->prod);
    DataBuf_Init(&ctx->pend);
    ctx->interp = interp;
    ctx->tclid = Tcl_GetCurrentThread();
    memset(ctx->evt_counts, 0, sizeof(ctx->evt_counts));
    /* Open library (DLL/.so/.dylib) in a portable way */
    ctx->handle = PDl_OpenFromObj(interp, libPathObj);
    if (!ctx->handle) {
        Tcl_Free(ctx);
        return TCL_ERROR;
    }
    /* Resolve required symbols; bail on first failure */
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
    /* create unique handle command */
    static unsigned long seq = 0;
    Tcl_Obj *name = Tcl_ObjPrintf("::ngspicetclbridge::s%lu", ++seq);
    Tcl_CreateObjCommand2(interp, Tcl_GetString(name), InstObjCmd, ctx, InstDeleteProc);
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
    /* provide */
    if (Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION, NULL) != TCL_OK) {
        return TCL_ERROR;
    }
    return TCL_OK;
}
