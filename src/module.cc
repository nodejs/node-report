#include "node_report.h"
#include <nan.h>

namespace node-report {

// Internal/static function declarations
static void OnFatalError(const char* location, const char* message);
bool OnUncaughtException(v8::Isolate* isolate);
#ifndef _WIN32
static void SignalDumpAsyncCallback(uv_async_t* handle);
inline void* ReportSignalThreadMain(void* unused);
static int StartWatchdogThread(void* (*thread_main)(void* unused));
static void RegisterSignalHandler(int signo, void (*handler)(int),
                                  struct sigaction* saved_sa);
static void RestoreSignalHandler(int signo, struct sigaction* saved_sa);
static void SignalDump(int signo);
static void SetupSignalHandler();
#endif

// Default node-report option settings
static unsigned int node-report_events = NR_APICALL;
static unsigned int node-report_core = 1;
static unsigned int node-report_verbose = 0;
#ifdef _WIN32  // signal trigger not supported on Windows
static unsigned int node-report_signal = 0;
#else  // trigger signal supported on Unix platforms and OSX
static unsigned int node-report_signal = SIGUSR2; // default signal is SIGUSR2
static int report_signal = 0;  // atomic for signal handling in progress
static uv_sem_t report_semaphore;  // semaphore for hand-off to watchdog
static uv_async_t node-report_trigger_async;  // async handle for event loop
static uv_mutex_t node_isolate_mutex;  // mutex for wachdog thread
static struct sigaction saved_sa;  // saved signal action
#endif

// State variables for v8 hooks and signal initialisation
static bool exception_hook_initialised = false;
static bool error_hook_initialised = false;
static bool signal_thread_initialised = false;

static v8::Isolate* node_isolate;

/*******************************************************************************
 * External JavaScript API for triggering a node-report
 *
 ******************************************************************************/
NAN_METHOD(TriggerReport) {
  Nan::HandleScope scope;
  v8::Isolate* isolate = info.GetIsolate();
  char filename[NR_MAXNAME + 1] = "";

  if (info[0]->IsString()) {
    // Filename parameter supplied
    Nan::Utf8String filename_parameter(info[0]);
    if (filename_parameter.length() < NR_MAXNAME) {
      snprintf(filename, sizeof(filename), "%s", *filename_parameter);
    } else {
      Nan::ThrowError("node-report: filename parameter is too long");
    }
  }
  if (node-report_events & NR_APICALL) {
    Triggernode-report(isolate, kJavaScript, "JavaScript API", __func__, filename);
    // Return value is the node-report filename
    info.GetReturnValue().Set(Nan::New(filename).ToLocalChecked());
  }
}

/*******************************************************************************
 * External JavaScript APIs for node-report configuration
 *
 ******************************************************************************/
NAN_METHOD(SetEvents) {
  Nan::Utf8String parameter(info[0]);
  v8::Isolate* isolate = info.GetIsolate();
  unsigned int previous_events = node-report_events; // save previous settings
  node-report_events = Processnode-reportEvents(*parameter);

  // If node-report newly requested for fatalerror, set up the V8 call-back
  if ((node-report_events & NR_FATALERROR) && (error_hook_initialised == false)) {
    isolate->SetFatalErrorHandler(OnFatalError);
    error_hook_initialised = true;
  }

  // If node-report newly requested for exceptions, tell V8 to capture stack trace and set up the callback
  if ((node-report_events & NR_EXCEPTION) && (exception_hook_initialised == false)) {
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);
    // The hook for uncaught exception won't get called unless the --abort_on_uncaught_exception option is set
    v8::V8::SetFlagsFromString("--abort_on_uncaught_exception", sizeof("--abort_on_uncaught_exception")-1);
    isolate->SetAbortOnUncaughtExceptionCallback(OnUncaughtException);
    exception_hook_initialised = true;
  }

#ifndef _WIN32
  // If node-report newly requested on external user signal set up watchdog thread and handler
  if ((node-report_events & NR_SIGNAL) && (signal_thread_initialised == false)) {
    SetupSignalHandler();
  }
  // If node-report no longer required on external user signal, reset the OS signal handler
  if (!(node-report_events & NR_SIGNAL) && (previous_events & NR_SIGNAL)) {
    RestoreSignalHandler(node-report_signal, &saved_sa);
  }
#endif
}
NAN_METHOD(SetCoreDump) {
  Nan::Utf8String parameter(info[0]);
  node-report_core = Processnode-reportCoreSwitch(*parameter);
}
NAN_METHOD(SetSignal) {
#ifndef _WIN32
  Nan::Utf8String parameter(info[0]);
  unsigned int previous_signal = node-report_signal; // save previous setting
  node-report_signal = Processnode-reportSignal(*parameter);

  // If signal event active and selected signal has changed, switch the OS signal handler
  if ((node-report_events & NR_SIGNAL) && (node-report_signal != previous_signal)) {
    RestoreSignalHandler(previous_signal, &saved_sa);
    RegisterSignalHandler(node-report_signal, SignalDump, &saved_sa);
  }
#endif
}
NAN_METHOD(SetFileName) {
  Nan::Utf8String parameter(info[0]);
  Processnode-reportFileName(*parameter);
}
NAN_METHOD(SetDirectory) {
  Nan::Utf8String parameter(info[0]);
  Processnode-reportDirectory(*parameter);
}
NAN_METHOD(SetVerbose) {
  Nan::Utf8String parameter(info[0]);
  node-report_verbose = Processnode-reportVerboseSwitch(*parameter);
}

/*******************************************************************************
 * Callbacks for triggering node-report on fatal error, uncaught exception and
 * external signals
 ******************************************************************************/
static void OnFatalError(const char* location, const char* message) {
  if (location) {
    fprintf(stderr, "FATAL ERROR: %s %s\n", location, message);
  } else {
    fprintf(stderr, "FATAL ERROR: %s\n", message);
  }
  // Trigger node-report if requested
  if (node-report_events & NR_FATALERROR) {
    Triggernode-report(Isolate::GetCurrent(), kFatalError, message, location, nullptr);
  }
  fflush(stderr);
  if (node-report_core) {
    raise(SIGABRT); // core dump requested (default)
  } else {
    exit(1); // user specified that no core dump is wanted, just exit
  }
}

bool OnUncaughtException(v8::Isolate* isolate) {
   // Trigger node-report if required
  if (node-report_events & NR_EXCEPTION) {
    Triggernode-report(isolate, kException, "exception", __func__, nullptr);
  } 
  return node-report_core;
}

#ifndef _WIN32
static void SignalDumpInterruptCallback(Isolate* isolate, void* data) {
  if (report_signal != 0) {
    if (node-report_verbose) {
      fprintf(stdout, "node-report: SignalDumpInterruptCallback handling signal\n");
    }
    if (node-report_events & NR_SIGNAL) {
      if (node-report_verbose) {
        fprintf(stdout, "node-report: SignalDumpInterruptCallback triggering node-report\n");
      }
      Triggernode-report(isolate, kSignal_JS,
                        node::signo_string(report_signal), __func__, nullptr);
    }
    report_signal = 0;
  }
}
static void SignalDumpAsyncCallback(uv_async_t* handle) {
  if (report_signal != 0) {
    if (node-report_verbose) {
      fprintf(stdout, "node-report: SignalDumpAsyncCallback handling signal\n");
    }
    if (node-report_events & NR_SIGNAL) {
      if (node-report_verbose) {
        fprintf(stdout, "node-report: SignalDumpAsyncCallback triggering node-report\n");
      }
      Triggernode-report(Isolate::GetCurrent(), kSignal_UV,
                        node::signo_string(report_signal), __func__, nullptr);
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
static void RegisterSignalHandler(int signo, void (*handler)(int),
                                  struct sigaction* saved_sa) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigfillset(&sa.sa_mask);  // mask all signals while in the handler
  sigaction(signo, &sa, saved_sa);
}

// Utility function to restore an OS signal handler to its previous setting
static void RestoreSignalHandler(int signo, struct sigaction* saved_sa) {
  sigaction(signo, saved_sa, nullptr);
}

// Raw signal handler for triggering a node-report - runs on an arbitrary thread
static void SignalDump(int signo) {
  // Check atomic for node-report already pending, storing the signal number
  if (__sync_val_compare_and_swap(&report_signal, 0, signo) == 0) {
    uv_sem_post(&report_semaphore);  // Hand-off to watchdog thread
  }
}

// Utility function to start a watchdog thread - used for processing signals
static int StartWatchdogThread(void* (*thread_main)(void* unused)) {
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
    fprintf(stderr, "node-report: StartWatchdogThread pthread_create() failed: %s\n", strerror(err));
    fflush(stderr);
    return -err;
  }
  return 0;
}

// Watchdog thread implementation for signal-triggered node-report
inline void* ReportSignalThreadMain(void* unused) {
  for (;;) {
    uv_sem_wait(&report_semaphore);
    if (node-report_verbose) {
      fprintf(stdout, "node-report: signal %s received\n", node::signo_string(report_signal));
    }
    uv_mutex_lock(&node_isolate_mutex);
    if (auto isolate = node_isolate) {
      // Request interrupt callback for running JavaScript code
      isolate->RequestInterrupt(SignalDumpInterruptCallback, nullptr);
      // Event loop may be idle, so also request an async callback
      uv_async_send(&node-report_trigger_async);
    }
    uv_mutex_unlock(&node_isolate_mutex);
  }
  return nullptr;
}

// Utility function to initialise signal handlers and threads
static void SetupSignalHandler() {
  int rc = uv_sem_init(&report_semaphore, 0);
  if (rc != 0) {
    fprintf(stderr, "node-report: initialization failed, uv_sem_init() returned %d\n", rc);
    Nan::ThrowError("node-report: initialization failed, uv_sem_init() returned error\n");
  }
  rc = uv_mutex_init(&node_isolate_mutex);
  if (rc != 0) {
    fprintf(stderr, "node-report: initialization failed, uv_mutex_init() returned %d\n", rc);
    Nan::ThrowError("node-report: initialization failed, uv_mutex_init() returned error\n");
  }

  if (StartWatchdogThread(ReportSignalThreadMain) == 0) {
    rc = uv_async_init(uv_default_loop(), &node-report_trigger_async, SignalDumpAsyncCallback);
    if (rc != 0) {
      fprintf(stderr, "node-report: initialization failed, uv_async_init() returned %d\n", rc);
      Nan::ThrowError("node-report: initialization failed, uv_async_init() returned error\n");
    }
    uv_unref(reinterpret_cast<uv_handle_t*>(&node-report_trigger_async));
    RegisterSignalHandler(node-report_signal, SignalDump, &saved_sa);
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
  SetVersionString(isolate);
  SetCommandLine();

  const char* verbose_switch = secure_getenv("NODE_REPORT_VERBOSE");
  if (verbose_switch != nullptr) {
    node-report_verbose = Processnode-reportVerboseSwitch(verbose_switch);
  }
  const char* trigger_events = secure_getenv("NODE_REPORT_EVENTS");
  if (trigger_events != nullptr) {
    node-report_events = Processnode-reportEvents(trigger_events);
  }
  const char* core_dump_switch = secure_getenv("NODE_REPORT_COREDUMP");
  if (core_dump_switch != nullptr) {
    node-report_core = Processnode-reportCoreSwitch(core_dump_switch);
  }
  const char* trigger_signal = secure_getenv("NODE_REPORT_SIGNAL");
  if (trigger_signal != nullptr) {
    node-report_signal = Processnode-reportSignal(trigger_signal);
  }
  const char* report_name = secure_getenv("NODE_REPORT_FILENAME");
  if (report_name != nullptr) {
    Processnode-reportFileName(report_name);
  }
  const char* directory_name = secure_getenv("NODE_REPORT_DIRECTORY");
  if (directory_name != nullptr) {
    Processnode-reportDirectory(directory_name);
  }

  // If node-report requested for fatalerror, set up the V8 call-back
  if (node-report_events & NR_FATALERROR) {
    isolate->SetFatalErrorHandler(OnFatalError);
    error_hook_initialised = true;
  }

  // If node-report requested for exceptions, tell V8 to capture stack trace and set up the callback
  if (node-report_events & NR_EXCEPTION) {
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);
    // The hook for uncaught exception won't get called unless the --abort_on_uncaught_exception option is set
    v8::V8::SetFlagsFromString("--abort_on_uncaught_exception", sizeof("--abort_on_uncaught_exception")-1);
    isolate->SetAbortOnUncaughtExceptionCallback(OnUncaughtException);
    exception_hook_initialised = true;
  }

#ifndef _WIN32
  // If node-report requested on external user signal set up watchdog thread and callbacks
  if (node-report_events & NR_SIGNAL) {
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

  if (node-report_verbose) {
#ifdef _WIN32
    fprintf(stdout, "node-report: initialization complete, event flags: %#x core flag: %#x\n",
            node-report_events, node-report_core);
#else
    fprintf(stdout, "node-report: initialization complete, event flags: %#x core flag: %#x signal flag: %#x\n",
            node-report_events, node-report_core, node-report_signal);
#endif
  }
}

NODE_MODULE(node-report, Initialize)

}  // namespace node-report

