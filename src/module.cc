#include <nan.h>
//#include <node.h>
#include "node_report.h"

// Internal/static function declarations
static void OnFatalError(const char* location, const char* message);
bool OnUncaughtException(v8::Isolate* isolate);
#ifndef _WIN32
static void SignalDumpAsyncCallback(uv_async_t* handle);
inline void* ReportSignalThreadMain(void* unused);
static int StartWatchdogThread(void *(*thread_main) (void* unused));
static void RegisterSignalHandler(int signal, void (*handler)(int signal), bool reset_handler);
static void SignalDump(int signo);
#endif

// Default nodereport option settings
static unsigned int nodereport_events = NR_EXCEPTION + NR_FATALERROR + NR_SIGNAL + NR_JSAPICALL;
static unsigned int nodereport_core = 1;
static unsigned int nodereport_verbose = 0;

#ifdef _WIN32
static unsigned int nodereport_signal = 0; // no-op on Windows
#else  // signal support - on Unix/OSX only
static unsigned int nodereport_signal = SIGUSR2; // default signal is SIGUSR2
static int report_signal = 0;
static uv_sem_t report_semaphore;
static uv_async_t nodereport_trigger_async;
static uv_mutex_t node_isolate_mutex;
#endif

static v8::Isolate* node_isolate;

/*******************************************************************************
 * External JavaScript API for triggering a NodeReport
 *
 ******************************************************************************/
NAN_METHOD(TriggerReport) {
  Nan::HandleScope scope;
  v8::Isolate* isolate = Isolate::GetCurrent();
  char filename[48] = "";

  if (info[0]->IsString()) {
    // Filename parameter supplied
    Nan::Utf8String filename_parameter(info[0]->ToString());
    if (filename_parameter.length() < 48) {
      strcpy(filename, *filename_parameter);
    } else {
      Nan::ThrowSyntaxError("TriggerReport: filename parameter is too long (max 48 characters)");
    }
  }
  if (info[0]->IsFunction()) {
    // Callback parameter supplied
    Nan::Callback callback(info[0].As<Function>());
    // Creates a new Object on the V8 heap
    Local<Object> obj = Object::New(isolate);
    obj->Set(String::NewFromUtf8(isolate, "number"), Number::New(isolate, 54));
    Local<Value> argv[1];
    argv[0] = obj;
    // Invoke the callback, passing the object in argv
    callback.Call(1, argv);
  }

  TriggerNodeReport(isolate, kJavaScript, "JavaScript API", "TriggerReport (nodereport/src/module.cc)", filename);
  // Return value is the NodeReport filename
  info.GetReturnValue().Set(Nan::New(filename).ToLocalChecked());
}

/*******************************************************************************
 * External JavaScript APIs for nodereport configuration
 *
 ******************************************************************************/
NAN_METHOD(SetEvents) {
  Nan::Utf8String parameter(info[0]->ToString());
  v8::Isolate* isolate = node_isolate;
  unsigned int previous_events = nodereport_events; // save previous settings
  nodereport_events = ProcessNodeReportEvents(*parameter);

  // If NodeReport newly requested for fatalerror, set up the V8 call-back
  if ((nodereport_events & NR_FATALERROR) && !(previous_events & NR_FATALERROR)) {
    isolate->SetFatalErrorHandler(OnFatalError);
  }

  // If NodeReport newly requested for exceptions, tell V8 to capture stack trace and set up the callback
  if ((nodereport_events & NR_EXCEPTION) && !(previous_events & NR_EXCEPTION)) {
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);
    // The hook for uncaught exception won't get called unless the --abort_on_uncaught_exception option is set
    v8::V8::SetFlagsFromString("--abort_on_uncaught_exception", sizeof("--abort_on_uncaught_exception")-1);
    isolate->SetAbortOnUncaughtExceptionCallback(OnUncaughtException);
  }

#ifndef _WIN32
  // If NodeReport newly requested on external user signal set up watchdog thread and callbacks
  if ((nodereport_events & NR_SIGNAL) && !(previous_events & NR_SIGNAL)) {
    uv_sem_init(&report_semaphore, 0);
    if (StartWatchdogThread(ReportSignalThreadMain) == 0) {
      uv_async_init(uv_default_loop(), &nodereport_trigger_async, SignalDumpAsyncCallback);
      uv_unref(reinterpret_cast<uv_handle_t*>(&nodereport_trigger_async));
      RegisterSignalHandler(nodereport_signal, SignalDump, false);
    }
  }
#endif
}
NAN_METHOD(SetCoreDump) {
  Nan::Utf8String parameter(info[0]->ToString());
  nodereport_events = ProcessNodeReportCoreSwitch(*parameter);
}
NAN_METHOD(SetSignal) {
  Nan::Utf8String parameter(info[0]->ToString());
  nodereport_events = ProcessNodeReportSignal(*parameter);
}
NAN_METHOD(SetFileName) {
  Nan::Utf8String parameter(info[0]->ToString());
  ProcessNodeReportFileName(*parameter);
}
NAN_METHOD(SetDirectory) {
  Nan::Utf8String parameter(info[0]->ToString());
  ProcessNodeReportDirectory(*parameter);
}
NAN_METHOD(SetVerbose) {
  Nan::Utf8String parameter(info[0]->ToString());
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
    raise(SIGABRT); // core dump requested
  } else {
    exit(0); // no core dump requested
  }
}

bool OnUncaughtException(v8::Isolate* isolate) {
   // Trigger NodeReport if required
  if (nodereport_events & NR_EXCEPTION) {
    TriggerNodeReport(Isolate::GetCurrent(), kException, "exception", "OnUncaughtException (nodereport/src/module.cc)", NULL);
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
    fprintf(stderr,"SignalDumpInterruptCallback - handling signal\n");
    if (nodereport_events & NR_SIGNAL) {
      if (nodereport_verbose) {
        fprintf(stderr,"SignalDumpInterruptCallback - triggering NodeReport\n");
      }
      TriggerNodeReport(Isolate::GetCurrent(), kSignal_JS,
                        node::signo_string(*(static_cast<int *>(data))),
                        "node::SignalDumpInterruptCallback()", NULL);
    }
    report_signal = 0;
  }
}
static void SignalDumpAsyncCallback(uv_async_t* handle) {
  if (report_signal != 0) {
    fprintf(stderr,"SignalDumpAsyncCallback - handling signal\n");
    if (nodereport_events & NR_SIGNAL) {
      if (nodereport_verbose) {
        fprintf(stderr,"SignalDumpAsyncCallback - triggering NodeReport\n");
      }
      size_t signo_data = reinterpret_cast<size_t>(handle->data);
      TriggerNodeReport(Isolate::GetCurrent(), kSignal_UV,
                        node::signo_string(static_cast<int>(signo_data)),
                        "node::SignalDumpAsyncCallback()", NULL);
    }
    report_signal = 0;
  }
}

static void RegisterSignalHandler(int signal, void (*handler)(int signal), bool reset_handler = false) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sa.sa_flags = reset_handler ? SA_RESETHAND : 0;
  sigfillset(&sa.sa_mask);
  sigaction(signal, &sa, nullptr);
}

// Raw signal handler for triggering a NodeReport - runs on an arbitrary thread
static void SignalDump(int signo) {
  // Check atomic for NodeReport already pending, storing the signal number
  if (__sync_val_compare_and_swap(&report_signal, 0, signo) == 0) {
    uv_sem_post(&report_semaphore);  // Hand-off to watchdog thread
  }
}

// Watchdog thread implementation for signal-triggered NodeReport
inline void* ReportSignalThreadMain(void* unused) {
  for (;;) {
    uv_sem_wait(&report_semaphore);
    if (nodereport_verbose) {
      fprintf(stderr, "Signal %s received by nodereport module\n", node::signo_string(report_signal));
    }
    uv_mutex_lock(&node_isolate_mutex);
    if (auto isolate = node_isolate) {
      // Request interrupt callback for running JavaScript code
      isolate->RequestInterrupt(SignalDumpInterruptCallback, &report_signal);
      // Event loop may be idle, so also request an async callback
      size_t signo_data = static_cast<size_t>(report_signal);
      nodereport_trigger_async.data = reinterpret_cast<void *>(signo_data);
      uv_async_send(&nodereport_trigger_async);
    }
    uv_mutex_unlock(&node_isolate_mutex);
  }
  return nullptr;
}

static int StartWatchdogThread(void *(*thread_main) (void* unused)) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);

  pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  sigset_t sigmask;
  sigfillset(&sigmask);
  pthread_sigmask(SIG_SETMASK, &sigmask, &sigmask);
  pthread_t thread;
  const int err = pthread_create(&thread, &attr, thread_main, nullptr);
  pthread_sigmask(SIG_SETMASK, &sigmask, nullptr);
  pthread_attr_destroy(&attr);
  if (err != 0) {
    fprintf(stderr, "nodereport: pthread_create: %s\n", strerror(err));
    fflush(stderr);
    return -err;
  }
  return 0;
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

  const char* trigger_events = getenv("NODEREPORT_EVENTS");
  if (trigger_events != NULL) {
    nodereport_events = ProcessNodeReportEvents(trigger_events);
  }
  const char* core_dump_switch = getenv("NODEREPORT_COREDUMP");
  if (core_dump_switch != NULL) {
    nodereport_core = ProcessNodeReportCoreSwitch(core_dump_switch);
  }
  const char* trigger_signal = getenv("NODEREPORT_SIGNAL");
  if (trigger_signal != NULL) {
    nodereport_signal = ProcessNodeReportSignal(trigger_signal);
  }
  const char* report_name = getenv("NODEREPORT_FILENAME");
  if (report_name != NULL) {
    ProcessNodeReportFileName(report_name);
  }
  const char* directory_name = getenv("NODEREPORT_DIRECTORY");
  if (directory_name != NULL) {
    ProcessNodeReportDirectory(directory_name);
  }
  const char* verbose_switch = getenv("NODEREPORT_VERBOSE");
  if (verbose_switch != NULL) {
	  nodereport_verbose = ProcessNodeReportVerboseSwitch(verbose_switch);
  }

  // If NodeReport requested for fatalerror, set up the V8 call-back
  if (nodereport_events & NR_FATALERROR) {
    isolate->SetFatalErrorHandler(OnFatalError);
  }

  // If NodeReport requested for exceptions, tell V8 to capture stack trace and set up the callback
  if (nodereport_events & NR_EXCEPTION) {
    isolate->SetCaptureStackTraceForUncaughtExceptions(true, 32, v8::StackTrace::kDetailed);
    // The hook for uncaught exception won't get called unless the --abort_on_uncaught_exception option is set
    v8::V8::SetFlagsFromString("--abort_on_uncaught_exception", sizeof("--abort_on_uncaught_exception")-1);
    isolate->SetAbortOnUncaughtExceptionCallback(OnUncaughtException);
  }

#ifndef _WIN32
  // If NodeReport requested on external user signal set up watchdog thread and callbacks
  if (nodereport_events & NR_SIGNAL) {
    uv_sem_init(&report_semaphore, 0);
    if (StartWatchdogThread(ReportSignalThreadMain) == 0) {
      uv_async_init(uv_default_loop(), &nodereport_trigger_async, SignalDumpAsyncCallback);
      uv_unref(reinterpret_cast<uv_handle_t*>(&nodereport_trigger_async));
      RegisterSignalHandler(nodereport_signal, SignalDump, false);
    }
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
    fprintf(stdout, "Initialized nodereport module, event flags: %#x core flag: %#x\n",
            nodereport_events, nodereport_core);
#else
    fprintf(stdout, "Initialized nodereport module, event flags: %#x core flag: %#x signal flag: %#x\n",
            nodereport_events, nodereport_core, nodereport_signal);
#endif
  }
}

NODE_MODULE(nodereport, Initialize)
