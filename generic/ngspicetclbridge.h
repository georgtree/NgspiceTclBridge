
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <tclThread.h>
#include <stdint.h>
#include <tcl.h>

#define XSPICE 1
#include "sharedspice.h"
#include "portable_dl.h"

enum CallbacksIds { SEND_CHAR, SEND_STAT, CONTROLLED_EXIT, SEND_DATA, SEND_INIT_DATA, BG_THREAD_RUNNING, NUM_EVTS };
typedef enum { NGSPICE_WAIT_OK, NGSPICE_WAIT_TIMEOUT, NGSPICE_WAIT_ABORTED } wait_rc;
/* Dvec flags from dvec.h of ngspice source files */
enum dvec_flags {
    VF_REAL = (1 << 0),      /* The data is real. */
    VF_COMPLEX = (1 << 1),   /* The data is complex. */
    VF_ACCUM = (1 << 2),     /* writedata should save this vector. */
    VF_PLOT = (1 << 3),      /* writedata should incrementally plot it. */
    VF_PRINT = (1 << 4),     /* writedata should print this vector. */
    VF_MINGIVEN = (1 << 5),  /* The v_minsignal value is valid. */
    VF_MAXGIVEN = (1 << 6),  /* The v_maxsignal value is valid. */
    VF_PERMANENT = (1 << 7), /* Don't garbage collect this vector. */
    VF_EVENT_NODE = (1 << 8) /* Derived from and XSPICE event node. */
};

enum vector_types {
    SV_NOTYPE,
    SV_TIME,
    SV_FREQUENCY,
    SV_VOLTAGE,
    SV_CURRENT,
    SV_VOLTAGE_DENSITY,
    SV_CURRENT_DENSITY,
    SV_SQR_VOLTAGE_DENSITY,
    SV_SQR_CURRENT_DENSITY,
    SV_SQR_VOLTAGE,
    SV_SQR_CURRENT,
    SV_POLE,
    SV_ZERO,
    SV_SPARAM,
    SV_TEMP,
    SV_RES,
    SV_IMPEDANCE,
    SV_ADMITTANCE,
    SV_POWER,
    SV_PHASE,
    SV_DB,
    SV_CAPACITANCE,
    SV_CHARGE
};

typedef enum {
    NGSTATE_IDLE = 0,          // no bg thread active
    NGSTATE_STARTING_BG,       // bg_run issued, waiting for bg_started=1
    NGSTATE_BG_ACTIVE,         // bg thread running normally
    NGSTATE_STOPPING_BG,       // bg_halt issued and bg thread is winding down
    NGSTATE_DEAD               // teardown/abort/destroy
} NgState;

//** define data snaps for saving data during ngspice callbacks
typedef struct {
    int veccount;
    struct {
        char *name;
        int number;
        int is_real;
    } *vecs;
} InitSnap;

typedef struct {
    char *name;
    int is_complex;
    double creal, cimag;
} DataCell;

typedef struct {
    int veccount;
    DataCell *vecs;   // length = veccount
} DataRow;

typedef struct {
    DataRow *rows;
    size_t count, cap;
} DataBuf;

//** define message queue
typedef struct {
    char **items;
    size_t count, cap;
} MsgQueue;

//** define commands buffer
typedef struct PendingCmd {
    char *cmd;                // Tcl string dup’d (must free later)
    int  capture;             // 0 or 1, mirrors -capture mode
    struct PendingCmd *next;  // linked list
} PendingCmd;

//** define functions pointers
typedef int (*ngSpice_Init_t)(SendChar *, SendStat *, ControlledExit *, SendData *, SendInitData *, BGThreadRunning *,
                              void *);
typedef int (*ngSpice_Init_Sync_t)(GetVSRCData *, GetISRCData *, GetSyncData *, int *, void *);
typedef int (*ngSpice_Command_t)(char *);
typedef pvector_info (*ngGet_Vec_Info_t)(char *);
typedef char *(*ngCM_Input_Path_t)(const char *);
typedef pevt_shared_data (*ngGet_Evt_NodeInfo_t)(char *);
typedef char **(*ngSpice_AllEvtNodes_t)(void);
typedef int (*ngSpice_Init_Evt_t)(SendEvtData *, SendInitEvtData *, void *);
typedef int (*ngSpice_Circ_t)(char **);
typedef char *(*ngSpice_CurPlot_t)(void);
typedef char **(*ngSpice_AllPlots_t)(void);
typedef char **(*ngSpice_AllVecs_t)(char *);
typedef NG_BOOL (*ngSpice_running_t)(void);
typedef NG_BOOL (*ngSpice_SetBkpt_t)(double);
typedef int (*ngSpice_nospinit_t)(void);
typedef int (*ngSpice_nospiceinit_t)(void);

//** define ngspice per-instance context structure
typedef struct {
    /*------------------------------------------------------------------------------------------------------------------
     * Dynamic library and function pointers (ngspice shared library symbols)
     *------------------------------------------------------------------------------------------------------------------*/
    PDlHandle handle;                             /* Dynamic library handle (libngspice.so / .dll) */
    ngSpice_Init_t ngSpice_Init;                  /* Classic ngspice initialization entry */
    ngSpice_Init_Sync_t ngSpice_Init_Sync;        /* Synchronous initialization (newer API) */
    ngSpice_Command_t ngSpice_Command;            /* Main command entry point */
    ngGet_Vec_Info_t ngGet_Vec_Info;              /* Query vector info (type, length, data) */
    ngCM_Input_Path_t ngCM_Input_Path;            /* Optional input path hook */
    ngGet_Evt_NodeInfo_t ngGet_Evt_NodeInfo;      /* Event-driven node information query */
    ngSpice_AllEvtNodes_t ngSpice_AllEvtNodes;    /* Enumerate all event-driven nodes */
    ngSpice_Init_Evt_t ngSpice_Init_Evt;          /* Event-mode initialization */
    ngSpice_Circ_t ngSpice_Circ;                  /* Send entire circuit deck */
    ngSpice_CurPlot_t ngSpice_CurPlot;            /* Get current plot name */
    ngSpice_AllPlots_t ngSpice_AllPlots;          /* Enumerate all available plots */
    ngSpice_AllVecs_t ngSpice_AllVecs;            /* Enumerate all vectors in a given plot */
    ngSpice_running_t ngSpice_running;            /* Query if background thread is active */
    ngSpice_SetBkpt_t ngSpice_SetBkpt;            /* Set breakpoint callback */
    ngSpice_nospinit_t ngSpice_nospinit;          /* Disable built-in ngspice init */
    ngSpice_nospiceinit_t ngSpice_nospiceinit;    /* Alternative init disable entry */

    /*------------------------------------------------------------------------------------------------------------------
     * Core synchronization and Tcl linkage
     *------------------------------------------------------------------------------------------------------------------*/
    Tcl_Mutex mutex;                              /* Protects general shared data (vectors, msgq, etc.) */
    Tcl_Condition cond;                           /* Signals data arrival or waitevent wake-up */
    Tcl_Interp *interp;                           /* Owning Tcl interpreter */
    Tcl_ThreadId tclid;                           /* Tcl thread ID of owning interpreter */

    /*------------------------------------------------------------------------------------------------------------------
     * Simulation data and initialization state
     *------------------------------------------------------------------------------------------------------------------*/
    InitSnap *init_snap;                          /* One-shot vector metadata snapshot (SEND_INIT_DATA) */
    DataBuf prod;                                 /* Primary data buffer — rows appended by ngspice thread */
    DataBuf pend;                                 /* Reserved future buffer (unused or staging) */

    Tcl_Obj *vectorData;                          /* Tcl dict: vector name → list(values) */
    Tcl_Obj *vectorInit;                          /* Tcl dict: vector name → {number N real 0/1} */

    /*------------------------------------------------------------------------------------------------------------------
     * Event and message tracking
     *------------------------------------------------------------------------------------------------------------------*/
    MsgQueue msgq;                                /* Async message queue for log/status lines */
    uint64_t evt_counts[NUM_EVTS];                /* Per-callback counters */
    uint64_t gen;                                 /* Generation number (run_id) for event validation */
    int new_run_pending;                          /* Marks pending new run between INIT/DATA callbacks */

    int destroying;                               /* True while context teardown in progress */
    int quitting;                                 /* True while sending "quit" command to ngspice */
    int aborting;                                 /* Soft abort flag for waitevent unblocking */
    int skip_dlclose;                             /* True to skip dlclose() on unsafe shutdown */

    /*------------------------------------------------------------------------------------------------------------------
     * Background (bg_run) thread coordination
     *------------------------------------------------------------------------------------------------------------------*/
    int bg_started;                               /* Set after first “started” callback received */
    int bg_ended;                                 /* Set after “ended” callback or post-quit */
    Tcl_Mutex bg_mu;                              /* Protects bg_started/bg_ended/state transitions */
    Tcl_Condition bg_cv;                          /* Signaled when BGThreadRunningCallback fires */

    /*------------------------------------------------------------------------------------------------------------------
     * Controlled exit synchronization (used by ControlledExitCallback)
     *------------------------------------------------------------------------------------------------------------------*/
    int exited;                                   /* 1 after ControlledExitCallback() runs */
    Tcl_Mutex exit_mu;                            /* Protects 'exited' flag */
    Tcl_Condition exit_cv;                        /* Signaled to wake teardown waiting for exit */

    /*------------------------------------------------------------------------------------------------------------------
     * Command capture window (used during "command -capture")
     *------------------------------------------------------------------------------------------------------------------*/
    int cap_active;                               /* 0/1 — true while capturing ngspice output */
    MsgQueue capq;                                /* Temporary capture buffer for stdout/stderr */

    /*------------------------------------------------------------------------------------------------------------------
     * Command queue and global run state
     *------------------------------------------------------------------------------------------------------------------*/
    NgState state;                                /* High-level simulator state (IDLE, STARTING_BG, etc.) */
    PendingCmd *pending_head;                     /* Head of queued Tcl commands awaiting safe dispatch */
    PendingCmd *pending_tail;                     /* Tail of queued Tcl commands */
    Tcl_Mutex cmd_mu;                             /* Protects pending command queue */
} NgSpiceContext;

//** Define a Tcl event record
typedef struct {
    Tcl_Event header;
    NgSpiceContext *ctx;
    int callbackId;
    uint64_t gen;
} NgSpiceEvent;
