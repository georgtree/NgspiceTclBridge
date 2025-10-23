
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
    /* dl + symbols */
    PDlHandle handle;
    ngSpice_Init_t ngSpice_Init;
    ngSpice_Init_Sync_t ngSpice_Init_Sync;
    ngSpice_Command_t ngSpice_Command;
    ngGet_Vec_Info_t ngGet_Vec_Info;
    ngCM_Input_Path_t ngCM_Input_Path;
    ngGet_Evt_NodeInfo_t ngGet_Evt_NodeInfo;
    ngSpice_AllEvtNodes_t ngSpice_AllEvtNodes;
    ngSpice_Init_Evt_t ngSpice_Init_Evt;
    ngSpice_Circ_t ngSpice_Circ;
    ngSpice_CurPlot_t ngSpice_CurPlot;
    ngSpice_AllPlots_t ngSpice_AllPlots;
    ngSpice_AllVecs_t ngSpice_AllVecs;
    ngSpice_running_t ngSpice_running;
    ngSpice_SetBkpt_t ngSpice_SetBkpt;
    ngSpice_nospinit_t ngSpice_nospinit;
    ngSpice_nospiceinit_t ngSpice_nospiceinit;

    /* threading / state */
    Tcl_Mutex mutex;
    Tcl_Condition cond;
    Tcl_Interp *interp;
    Tcl_ThreadId tclid;

    InitSnap *init_snap; /* one-shot snapshot from callback */
    DataBuf prod;        /* ngspice thread appends rows here */
    DataBuf pend;        /* reserved for future use */

    Tcl_Obj *vectorData; /* dict: name -> list(values) */
    Tcl_Obj *vectorInit; /* dict: name -> {number N real 0/1} */

    MsgQueue msgq;
    uint64_t evt_counts[NUM_EVTS];
    int destroying;

    uint64_t gen;            // <— current generation (run_id)
    int new_run_pending;     // <— optional: for the one-shot auto-reset in INIT processing
} NgSpiceContext;

//** Define a Tcl event record
typedef struct {
    Tcl_Event header;
    NgSpiceContext *ctx;
    int callbackId;
    uint64_t gen;
} NgSpiceEvent;
