#include "node_report.h"
#include <nan.h>

namespace nodereport {

// Internal/static function declarations
static void OnFatalError(const char* location, const char* message);
bool OnUncaughtException(v8::Isolate* isolate);
#ifndef _WIN32
static void SignalDumpAsyncCallback(uv_async_t* handle);
inline void* ReportSignalThreadMain(void* unused);
static int StartWatchdogThread(void *(*thread_main) (void* unused));
static void RegisterSignalHandler(int signal, void (*handler)(int signal));
static void RestoreSignalHandler(int signal);
static void SignalDump(int signo);
static void SetupSignalHandler();
#endif

// Default nodereport option settings
static unsigned int nodereport_events = NR_APICALL;
static unsigned int nodereport_core = 1;
static unsigned int nodereport_verbose = 0;
#ifdef _WIN32  // trigger signal not supported on Windows
static unsigned int nodereport_signal = 0;
#else  // trigger signal supported on Unix platforms and OSX
static unsigned int nodereport_signal = SIGUSR2; // default signal is SIGUSR2
static int report_signal = 0;  // atomic for signal handling in progress
static uv_sem_t report_semaphore;  // semaphore for hand-off to watchdog
static uv_async_t nodereport_trigger_async;  // async handle for event loop
static uv_mutex_t node_isolate_mutex;  // mutex for wachdog thread
static struct sigaction saved_sa;  // saved signal action
#endif

// State variables for v8 hooks and signal initialisation
static bool exception_hook_initialised = false;
static bool error_hook_initialised = false;
static bool signal_thread_initialised = false;

static v8::Isolate* node_isolate;

/*******************************************************************************
 * External JavaScript API for triggering a NodeReport
 *
 ******************************************************************************/
NAN_METHOD(TriggerReport) {
  Nan::HandleScope scope;
  v8::Isolate* isolate = info.GetIsolate();
  char filename[NR_MAXNAME + 1] = "";

  if (info[0]->IsString()) {
    // Filename parameter supplied
    Nan::Utf8String filename_parameter(info[0]->ToString());
    if (filename_parameter.length() < NR_MAXNAME) {
      snprintf(filename, sizeof(filename), "%s", *filename_parameter);
    } else {
      Nan::ThrowSyntaxError("nodereport: filename parameter is too long");
    }
  }
  if (nodereport_events & NR_APICALL) {
    TriggerNodeReport(isolate, kJavaScript, "JavaScript API", "TriggerReport (nodereport/src/module.cc)", filename);
    // Return value is the NodeReport filename
    info.GetReturnValue().Set(Nan::New(filename).ToLocalChecked());
  }
}

/*******************************************************************************
 * External JavaScript APIs for nodereport configuration
 *
 ******************************************************************************/
NAN_METHOD(SetEvents) {
  Nan::Utf8String parameter(info[0]);
  v8::Isolate* isolate = node_isolate;
  unsigned int previous_events = nodereport_events; // save previous settings
  nodereport_events = ProcessNodeReportEvents(*parameter);

  // If NodeReport newly requested for fatalerror, set up the V8 call-back
  if ((nodereport_events & NR_FATALERROR) && (error_hook_initialised == false)) {
    isolate->SetFatalErrorHandler(OnFatalError);
    error_hook_initialised = true;
  }

  // If NodeReport newly requested for exceptions, tell V8 to capture stack trace and set up the callback
  if ((nodereport_events & NR_EXCEPTION) && (exception_hook_initialised == false)) {
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);
    // The hook for uncaught exception won't get called unless the --abort_on_uncaught_exception option is set
    v8::V8::SetFlagsFromString("--abort_on_uncaught_exception", sizeof("--abort_on_uncaught_exception")-1);
    isolate->SetAbortOnUncaughtExceptionCallback(OnUncaughtException);
    exception_hook_initialised = true;
  }

#ifndef _WIN32
  // If NodeReport newly requested on external user signal set up watchdog thread and handler
  if ((nodereport_events & NR_SIGNAL) && (signal_thread_initialised == false)) {
    SetupSignalHandler();
  }
  // If NodeReport no longer required on external user signal, reset the OS signal handler
  if (!(nodereport_events & NR_SIGNAL) && (previous_events & NR_SIGNAL)) {
    RestoreSignalHandler(nodereport_signal);
  }
#endif
}
NAN_METHOD(SetCoreDump) {
  Nan::Utf8String parameter(info[0]);
  nodereport_core = ProcessNodeReportCoreSwitch(*parameter);
}
NAN_METHOD(SetSignal) {
#ifndef _WIN32
  Nan::Utf8String parameter(info[0]);
  unsigned int previous_signal = nodereport_signal; // save previous setting
  nodereport_signal = ProcessNodeReportSignal(*parameter);

  // If signal event active and selected signal has changed, switch the OS signal handler
  if ((nodereport_events & NR_SIGNAL) && (nodereport_signal != previous_signal)) {
    RestoreSignalHandler(previous_signal);
    RegisterSignalHandler(nodereport_signal, SignalDump);
  }
#endif
}
NAN_METHOD(SetFileName) {
  Nan::Utf8String parameter(info[0]);
  ProcessNodeReportFileName(*parameter);
}
NAN_METHOD(SetDirectory) {
  Nan::Utf8String parameter(info[0]);
  ProcessNodeReportDirectory(*parameter);
}
NAN_METHOD(SetVerbose) {
  Nan::Utf8String parameter(info[0]);
  nodereport_verbose = ProcessNodeReportVerboseSwitch(*parameter);
}

/*******************************************************************************
 * Callbacks for triggering NodeReport on failure events (as configured)
 *  - fatal error
 *  - uncaught exception
 *  - signal
 ******************************************************************************/
static void OnFatalError(const char* location, const char* message) {
  if (location) {
    fprintf(stderr, "FATAL ERROR: %s %s\n", location, message);
  } else {
    fprintf(stderr, "FATAL ERROR: %s\n", message);
  }
  // Trigger NodeReport if requested
  if (nodereport_events & NR_FATALERROR) {
    TriggerNodeReport(Isolate::GetCurrent(), kFatalError, message, location, NULL);
  }
  fflush(stderr);
  if (nodereport_core) {
    raise(SIGABRT); // core dump requested (default)
  } else {
    exit(1); // user specified that no core dump is wanted, just exit
  }
}

bool OnUncaughtException(v8::Isolate* isolate) {
   // Trigger NodeReport if required
  if (nodereport_events & NR_EXCEPTION) {
    TriggerNodeReport(isolate, kException, "exception", "OnUncaughtException (nodereport/src/module.cc)", NULL);
  } 
  if (nodereport_core) {
    return true;
  } else {
    return false;
  }
}

#ifndef _WIN32
static void SignalDumpInterruptCallback(Isolate *isolate, void *data) {
  if (report_signal != 0) {
    if (nodereport_verbose) {
      fprintf(stdout,"nodereport: SignalDumpInterruptCallback handling signal\n");
    }
    if (nodereport_events & NR_SIGNAL) {
      if (nodereport_verbose) {
        fprintf(stdout,"nodereport: SignalDumpInterruptCallback triggering NodeReport\n");
      }
      TriggerNodeReport(isolate, kSignal_JS,
                        node::signo_string(report_signal),
                        "node::SignalDumpInterruptCallback()", NULL);
    }
    report_signal = 0;
  }
}
static void SignalDumpAsyncCallback(uv_async_t* handle) {
  if (report_signal != 0) {
    if (nodereport_verbose) {
      fprintf(stdout,"nodereport: SignalDumpAsyncCallback handling signal\n");
    }
    if (nodereport_events & NR_SIGNAL) {
      if (nodereport_verbose) {
        fprintf(stdout,"nodereport: SignalDumpAsyncCallback triggering NodeReport\n");
      }
      TriggerNodeReport(Isolate::GetCurrent(), kSignal_UV,
                        node::signo_string(report_signal),
                        "node::SignalDumpAsyncCallback()", NULL);
    }
    report_signal = 0;
  }
}

/*******************************************************************************
 * Utility functions for signal handling support (platforms except Windows)
 *  - RegisterSignalHandler() - register a raw OS signal handler
 *  - SignalDump() - implementation of raw OS signal handler
 *  - StartWatchdogThread() - create a watchdog thread
 *  - ReportSignalThreadMain() - implementation of watchdog thread
 *  - SetupSignalHandler() - initialisation of signal handlers and threads
 ******************************************************************************/
 // Utility function to register an OS signal handler
static void RegisterSignalHandler(int signal, void (*handler)(int signal)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigfillset(&sa.sa_mask);  // mask all signals while in the handler
  sigaction(signal, &sa, &saved_sa);
}

// Utility function to restore an OS signal handler to its previous setting
static void RestoreSignalHandler(int signal) {
  sigaction(signal, &saved_sa, nullptr);
}

// Raw signal handler for triggering a NodeReport - runs on an arbitrary thread
static void SignalDump(int signo) {
  // Check atomic for NodeReport already pending, storing the signal number
  if (__sync_val_compare_and_swap(&report_signal, 0, signo) == 0) {
    uv_sem_post(&report_semaphore);  // Hand-off to watchdog thread
  }
}

// Utility function to start a watchdog thread - used for processing signals
static int StartWatchdogThread(void *(*thread_main) (void* unused)) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  // Minimise the stack size, except on FreeBSD where the minimum is too low
#ifndef __FreeBSD__
  pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
#endif  // __FreeBSD__
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  sigset_t sigmask;
  sigfillset(&sigmask);
  pthread_sigmask(SIG_SETMASK, &sigmask, &sigmask);
  pthread_t thread;
  const int err = pthread_create(&thread, &attr, thread_main, nullptr);
  pthread_sigmask(SIG_SETMASK, &sigmask, nullptr);
  pthread_attr_destroy(&attr);
  if (err != 0) {
    fprintf(stderr, "nodereport: StartWatchdogThread pthread_create() failed: %s\n", strerror(err));
    fflush(stderr);
    return -err;
  }
  return 0;
}

// Watchdog thread implementation for signal-triggered NodeReport
inline void* ReportSignalThreadMain(void* unused) {
  for (;;) {
    uv_sem_wait(&report_semaphore);
    if (nodereport_verbose) {
      fprintf(stdout, "nodereport: signal %s received\n", node::signo_string(report_signal));
    }
    uv_mutex_lock(&node_isolate_mutex);
    if (auto isolate = node_isolate) {
      // Request interrupt callback for running JavaScript code
      isolate->RequestInterrupt(SignalDumpInterruptCallback, NULL);
      // Event loop may be idle, so also request an async callback
      uv_async_send(&nodereport_trigger_async);
    }
    uv_mutex_unlock(&node_isolate_mutex);
  }
  return nullptr;
}

// Utility function to initialise signal handlers and threads
static void SetupSignalHandler() {
  int rc = uv_sem_init(&report_semaphore, 0);
  if (rc != 0) {
    fprintf(stderr, "nodereport: initialization failed, uv_sem_init() returned %d\n", rc);
    Nan::ThrowError("nodereport: initialization failed, uv_sem_init() returned error\n");
  }
  rc = uv_mutex_init(&node_isolate_mutex);
  if (rc != 0) {
    fprintf(stderr, "nodereport: initialization failed, uv_mutex_init() returned %d\n", rc);
    Nan::ThrowError("nodereport: initialization failed, uv_mutex_init() returned error\n");
  }

  if (StartWatchdogThread(ReportSignalThreadMain) == 0) {
    rc = uv_async_init(uv_default_loop(), &nodereport_trigger_async, SignalDumpAsyncCallback);
    if (rc != 0) {
      fprintf(stderr, "nodereport: initialization failed, uv_async_init() returned %d\n", rc);
      Nan::ThrowError("nodereport: initialization failed, uv_async_init() returned error\n");
    }
    uv_unref(reinterpret_cast<uv_handle_t*>(&nodereport_trigger_async));
    RegisterSignalHandler(nodereport_signal, SignalDump);
    signal_thread_initialised = true;
  }
}
#endif

/*******************************************************************************
 * Native module initializer function, called when the module is require'd
 *
 ******************************************************************************/
void Initialize(v8::Local<v8::Object> exports) {
  v8::Isolate* isolate = Isolate::GetCurrent();
  node_isolate = isolate;

  SetLoadTime();

  const char* verbose_switch = secure_getenv("NODEREPORT_VERBOSE");
  if (verbose_switch != NULL) {
    nodereport_verbose = ProcessNodeReportVerboseSwitch(verbose_switch);
  }
  const char* trigger_events = secure_getenv("NODEREPORT_EVENTS");
  if (trigger_events != NULL) {
    nodereport_events = ProcessNodeReportEvents(trigger_events);
  }
  const char* core_dump_switch = secure_getenv("NODEREPORT_COREDUMP");
  if (core_dump_switch != NULL) {
    nodereport_core = ProcessNodeReportCoreSwitch(core_dump_switch);
  }
  const char* trigger_signal = secure_getenv("NODEREPORT_SIGNAL");
  if (trigger_signal != NULL) {
    nodereport_signal = ProcessNodeReportSignal(trigger_signal);
  }
  const char* report_name = secure_getenv("NODEREPORT_FILENAME");
  if (report_name != NULL) {
    ProcessNodeReportFileName(report_name);
  }
  const char* directory_name = secure_getenv("NODEREPORT_DIRECTORY");
  if (directory_name != NULL) {
    ProcessNodeReportDirectory(directory_name);
  }

  // If NodeReport requested for fatalerror, set up the V8 call-back
  if (nodereport_events & NR_FATALERROR) {
    isolate->SetFatalErrorHandler(OnFatalError);
    error_hook_initialised = true;
  }

  // If NodeReport requested for exceptions, tell V8 to capture stack trace and set up the callback
  if (nodereport_events & NR_EXCEPTION) {
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);
    // The hook for uncaught exception won't get called unless the --abort_on_uncaught_exception option is set
    v8::V8::SetFlagsFromString("--abort_on_uncaught_exception", sizeof("--abort_on_uncaught_exception")-1);
    isolate->SetAbortOnUncaughtExceptionCallback(OnUncaughtException);
    exception_hook_initialised = true;
  }

#ifndef _WIN32
  // If NodeReport requested on external user signal set up watchdog thread and callbacks
  if (nodereport_events & NR_SIGNAL) {
    SetupSignalHandler();
  }
#endif

  exports->Set(Nan::New("triggerReport").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(TriggerReport)->GetFunction());
  exports->Set(Nan::New("setEvents").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(SetEvents)->GetFunction());
  exports->Set(Nan::New("setCoreDump").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(SetCoreDump)->GetFunction());
  exports->Set(Nan::New("setSignal").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(SetSignal)->GetFunction());
  exports->Set(Nan::New("setFileName").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(SetFileName)->GetFunction());
  exports->Set(Nan::New("setDirectory").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(SetDirectory)->GetFunction());
  exports->Set(Nan::New("setVerbose").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(SetVerbose)->GetFunction());

  if (nodereport_verbose) {
#ifdef _WIN32
    fprintf(stdout, "nodereport: initialization complete, event flags: %#x core flag: %#x\n",
            nodereport_events, nodereport_core);
#else
    fprintf(stdout, "nodereport: initialization complete, event flags: %#x core flag: %#x signal flag: %#x\n",
            nodereport_events, nodereport_core, nodereport_signal);
#endif
  }
}

NODE_MODULE(nodereport, Initialize)

}  // namespace nodereport

