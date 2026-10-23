/* Deterministic shared-ngspice fixture. No circuit solver: exercises the real bridge ABI,
 * including callbacks from a worker thread. Never install this library. */
#undef USE_TCL_STUBS
#define SHARED_MODULE
#define XSPICE 1
#include <tcl.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
typedef HANDLE WorkerThread;
#define WORKER_RESULT unsigned __stdcall
#else
#include <pthread.h>
typedef pthread_t WorkerThread;
#define WORKER_RESULT void *
#endif
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../generic/sharedspice.h"

static SendData *sendData;
static SendInitData *sendInit;
static BGThreadRunning *sendRunning;
static ControlledExit *sendExit;
static void *client;
static Tcl_Mutex mutex;
static WorkerThread worker;
static int finalized, terminalExit, burst;
static int joinable, running, halt, length, fast, pausedMode, resuming, nextPlot, plotId, failStart;
static Tcl_Condition stopped;
static char plotName[32] = "tran1";
static double times[10000], values[10000];
static ngcomplex_t complexValues[10000];
static char *names[] = {"time", "v(out)", "v(1)", NULL};
static vector_info info[3];

int ngSpice_Init(SendChar *a, SendStat *b, ControlledExit *c, SendData *d, SendInitData *e, BGThreadRunning *f,
                 void *g) {
    (void)a;
    (void)b;
    finalized = terminalExit = burst = 0;
    fast = pausedMode = resuming = nextPlot = plotId = failStart = 0;
    sendExit = c;
    sendData = d;
    sendInit = e;
    sendRunning = f;
    client = g;
    return 0;
}

static void Begin(int newPlot) {
    if (newPlot) {
        Tcl_MutexLock(&mutex);
        length = 0;
        snprintf(plotName, sizeof(plotName), "tran%d", ++plotId);
        Tcl_MutexUnlock(&mutex);
    }
    vecinfo columns[3] = {
        {0, names[0], true, NULL, NULL}, {1, names[1], true, NULL, NULL}, {2, names[2], false, NULL, NULL}};
    pvecinfo ptrs[] = {columns, columns + 1, columns + 2};
    vecinfoall metadata = {"Transient Analysis", "fixture", "", plotName, 3, ptrs};
    sendInit(&metadata, 0, client);
}

static void Point(void) {
    Tcl_MutexLock(&mutex);
    int n = length++;
    times[n] = n * 0.1;
    values[n] = n * 2.0;
    complexValues[n] = (ngcomplex_t){n * 2.0, -n * 3.0};
    Tcl_MutexUnlock(&mutex);
    vecvalues v[] = {{names[0], times[n], 0, true, false},
                     {names[1], values[n], 0, false, false},
                     {names[2], complexValues[n].cx_real, complexValues[n].cx_imag, false, true}};
    pvecvalues ptrs[] = {v, v + 1, v + 2};
    vecvaluesall row = {3, n, ptrs};
    sendData(&row, 3, 0, client);
}

/* The fixture deliberately uses native threads without Tcl_ExitThread. This exit handler
 * observes cleanup by the bridge, rather than hiding missing cleanup in the fixture. */
static void WorkerFinalized(ClientData unused) {
    (void)unused;
    finalized++;
}

static void JoinWorker(void) {
    if (!joinable) return;
#ifdef _WIN32
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
#else
    pthread_join(worker, NULL);
#endif
    joinable = 0;
}

static WORKER_RESULT Run(void *unused) {
    (void)unused;
    Tcl_CreateThreadExitHandler(WorkerFinalized, NULL);
    sendRunning(false, 0, client);
    Begin(!resuming || nextPlot);
    nextPlot = 0;
    for (int i = 0; i < (pausedMode ? 3 : (burst ? 1000 : 100)); i++) {
        Tcl_MutexLock(&mutex);
        int stop = halt;
        Tcl_MutexUnlock(&mutex);
        if (stop) {
            break;
        }
        Point();
        if (!burst) Tcl_Sleep(2);
    }
    Tcl_MutexLock(&mutex);
    while (pausedMode && !halt) {
        Tcl_ConditionWait(&stopped, &mutex, NULL);
    }
    running = 0;
    Tcl_MutexUnlock(&mutex);
    sendRunning(true, 0, client);
    if (terminalExit) {
        /* ngspice can report controlled_exit after bg_running(true). */
        Tcl_CreateThreadExitHandler(WorkerFinalized, NULL);
        sendExit(1, false, false, 0, client);
    }
    return 0;
}

static int commandClears;
int ngSpice_Command(char *command) {
    if (command == NULL) {
        ++commandClears;
        return 0;
    }
    if (!strcmp(command, "mock_clear_count")) return commandClears;

    if (failStart && (!strcmp(command, "bg_run") || !strcmp(command, "bg_resume"))) {
        failStart = 0;
        return 1;
    }
    if (!strcmp(command, "mock_finalized")) {
        JoinWorker();
        return finalized;
    } else if (!strcmp(command, "mock_terminal_exit")) {
        terminalExit = 1;
    } else if (!strcmp(command, "mock_burst")) {
        burst = 1;
    } else if (!strcmp(command, "mock_failstart")) {
        failStart = 1;
    } else if (!strcmp(command, "mock_begin")) {
        Begin(1);
    } else if (!strcmp(command, "mock_point")) {
        Point();
    } else if (!strcmp(command, "mock_pause")) {
        pausedMode = 1;
    } else if (!strcmp(command, "mock_resume_newplot")) {
        nextPlot = 1;
    } else if (!strcmp(command, "mock_fast")) {
        fast = 1;
    } else if (!strcmp(command, "run") || (!strcmp(command, "bg_run") && fast)) {
        int background = !strcmp(command, "bg_run");
        if (background) sendRunning(false, 0, client);
        Begin(1);
        for (int i = 0; i < 3; i++) {
            Point();
        }
        if (background) {
            sendRunning(true, 0, client);
        }            
    } else if (!strcmp(command, "bg_run") || !strcmp(command, "bg_resume")) {
        JoinWorker();
        resuming = !strcmp(command, "bg_resume");
        Tcl_MutexLock(&mutex);
        halt = 0;
        running = 1;
        Tcl_MutexUnlock(&mutex);
#ifdef _WIN32
        worker = (HANDLE)_beginthreadex(NULL, 0, Run, NULL, 0, NULL);
        joinable = worker != NULL;
#else
        joinable = pthread_create(&worker, NULL, Run, NULL) == 0;
#endif
        if (!joinable) {
            return 1;
        }
    } else if (!strcmp(command, "bg_halt") || !strcmp(command, "quit")) {
        Tcl_MutexLock(&mutex);
        halt = 1;
        Tcl_ConditionNotify(&stopped);
        Tcl_MutexUnlock(&mutex);
        JoinWorker();
        if (!strcmp(command, "quit")) {
            sendExit(0, false, true, 0, client);
        }
    }
    return 0;
}

pvector_info ngGet_Vec_Info(char *name) {
    for (int i = 0; i < 3; i++) {
        if (!strcmp(name, names[i])) {
            info[i] = (vector_info){
                names[i], i ? 3 : 1, i == 2 ? 2 : 1, i == 0 ? times : values, i == 2 ? complexValues : NULL, length};
            return info + i;
        }
    }
    return NULL;
}

int ngSpice_LockRealloc(void) {
    Tcl_MutexLock(&mutex);
    return 0;
}

int ngSpice_UnlockRealloc(void) {
    Tcl_MutexUnlock(&mutex);
    return 0;
}

NG_BOOL ngSpice_running(void) {
    Tcl_MutexLock(&mutex);
    int r = running;
    Tcl_MutexUnlock(&mutex);
    return r != 0;
}

int ngSpice_Init_Sync(GetVSRCData *a, GetISRCData *b, GetSyncData *c, int *d, void *e) { return 0; }

char *ngCM_Input_Path(const char *path) { return (char *)path; }

pevt_shared_data ngGet_Evt_NodeInfo(char *name) { return NULL; }

char **ngSpice_AllEvtNodes(void) {
    static char *empty[] = {NULL};
    return empty;
}

int ngSpice_Init_Evt(SendEvtData *a, SendInitEvtData *b, void *c) { return 0; }

int ngSpice_Circ(char **circuit) { return 0; }

char *ngSpice_CurPlot(void) { return "tran1"; }

char **ngSpice_AllPlots(void) {
    static char *plots[] = {"tran1", NULL};
    return plots;
}

char **ngSpice_AllVecs(char *plot) { return names; }

NG_BOOL ngSpice_SetBkpt(double value) { return true; }

int ngSpice_nospinit(void) { return 0; }

int ngSpice_nospiceinit(void) { return 0; }
