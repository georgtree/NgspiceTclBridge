/* Deterministic shared-ngspice fixture. No circuit solver: exercises the real bridge ABI,
 * including callbacks from a worker thread. Never install this library. */
#undef USE_TCL_STUBS
#define SHARED_MODULE
#define XSPICE 1
#include <tcl.h>
#include <stdbool.h>
#include <string.h>
#include "../generic/sharedspice.h"

static SendData *sendData;
static SendInitData *sendInit;
static BGThreadRunning *sendRunning;
static ControlledExit *sendExit;
static void *client;
static Tcl_Mutex mutex;
static Tcl_ThreadId worker;
static int joinable, running, halt, length;
static double times[10000], values[10000];
static ngcomplex_t complexValues[10000];
static char *names[] = {"time", "v(out)", "v(1)", NULL};
static vector_info info[3];

int ngSpice_Init(SendChar *a, SendStat *b, ControlledExit *c, SendData *d, SendInitData *e, BGThreadRunning *f,
                 void *g) {
    (void)a;
    (void)b;
    sendExit = c;
    sendData = d;
    sendInit = e;
    sendRunning = f;
    client = g;
    return 0;
}
static void Begin(void) {
    Tcl_MutexLock(&mutex);
    length = 0;
    Tcl_MutexUnlock(&mutex);
    vecinfo columns[3] = {
        {0, names[0], true, NULL, NULL}, {1, names[1], true, NULL, NULL}, {2, names[2], false, NULL, NULL}};
    pvecinfo ptrs[] = {columns, columns + 1, columns + 2};
    vecinfoall metadata = {"tran1", "fixture", "", "tran", 3, ptrs};
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
static Tcl_ThreadCreateType Run(ClientData unused) {
    (void)unused;
    sendRunning(false, 0, client);
    Begin();
    for (int i = 0; i < 100; i++) {
        Tcl_MutexLock(&mutex);
        int stop = halt;
        Tcl_MutexUnlock(&mutex);
        if (stop) {
            break;
        }
        Point();
        Tcl_Sleep(2);
    }
    Tcl_MutexLock(&mutex);
    running = 0;
    Tcl_MutexUnlock(&mutex);
    sendRunning(true, 0, client);
    Tcl_ExitThread(0);
    TCL_THREAD_CREATE_RETURN;
}
int ngSpice_Command(char *command) {
    if (!strcmp(command, "mock_begin")) {
        Begin();
    } else if (!strcmp(command, "mock_point")) {
        Point();
    } else if (!strcmp(command, "bg_run")) {
        if (joinable) {
            int result;
            Tcl_JoinThread(worker, &result);
            joinable = 0;
        }
        Tcl_MutexLock(&mutex);
        halt = 0;
        running = 1;
        Tcl_MutexUnlock(&mutex);
        joinable = Tcl_CreateThread(&worker, Run, NULL, TCL_THREAD_STACK_DEFAULT, TCL_THREAD_JOINABLE) == TCL_OK;
        if (!joinable) {
            return 1;
        }
    } else if (!strcmp(command, "bg_halt") || !strcmp(command, "quit")) {
        Tcl_MutexLock(&mutex);
        halt = 1;
        Tcl_MutexUnlock(&mutex);
        if (joinable) {
            int result;
            Tcl_JoinThread(worker, &result);
            joinable = 0;
        }
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
