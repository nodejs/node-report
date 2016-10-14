#include "node_report.h"
#include "node_version.h"
#include "v8.h"
#include "time.h"
#include "zlib.h"
#include "ares.h"

#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(_MSC_VER)
#include <strings.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#include <process.h>
#include <dbghelp.h>
#include <VersionHelpers.h>
#else
#include <sys/time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <inttypes.h>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/utsname.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace nodereport {

using v8::HeapSpaceStatistics;
using v8::HeapStatistics;
using v8::Isolate;
using v8::Local;
using v8::Message;
using v8::StackFrame;
using v8::StackTrace;
using v8::String;
using v8::V8;

// Internal/static function declarations
static void PrintVersionInformation(FILE* fp);
static void PrintJavaScriptStack(FILE* fp, Isolate* isolate, DumpEvent event, const char* location);
static void PrintStackFromStackTrace(FILE* fp, Isolate* isolate, DumpEvent event);
static void PrintStackFrame(FILE* fp, Isolate* isolate, Local<StackFrame> frame, int index, void* pc);
static void PrintNativeStack(FILE* fp);
#ifndef _WIN32
static void PrintResourceUsage(FILE* fp);
#endif
static void PrintGCStatistics(FILE* fp, Isolate* isolate);
static void PrintSystemInformation(FILE* fp, Isolate* isolate);
static void WriteInteger(FILE* fp, size_t value);

// Global variables
static int seq = 0;  // sequence number for NodeReport filenames
const char* v8_states[] = {"JS", "GC", "COMPILER", "OTHER", "EXTERNAL", "IDLE"};
static bool report_active = false; // recursion protection
static char report_filename[NR_MAXNAME + 1] = "";
static char report_directory[NR_MAXPATH + 1] = ""; // defaults to current working directory
#ifdef _WIN32
static SYSTEMTIME loadtime_tm_struct; // module load time
#else  // UNIX, OSX
static struct tm loadtime_tm_struct; // module load time
#endif

/*******************************************************************************
 * Functions to process nodereport configuration options:
 *   Trigger event selection
 *   Core dump yes/no switch
 *   Trigger signal selection
 *   NodeReport filename
 *   NodeReport directory
 *   Verbose mode
 ******************************************************************************/
unsigned int ProcessNodeReportEvents(const char* args) {
  // Parse the supplied event types
  unsigned int event_flags = 0;
  const char* cursor = args;
  while (*cursor != '\0') {
    if (!strncmp(cursor, "exception", sizeof("exception") - 1)) {
      event_flags |= NR_EXCEPTION;
      cursor += sizeof("exception") - 1;
    } else if (!strncmp(cursor, "fatalerror", sizeof("fatalerror") - 1)) {
       event_flags |= NR_FATALERROR;
       cursor += sizeof("fatalerror") - 1;
    } else if (!strncmp(cursor, "signal", sizeof("signal") - 1)) {
      event_flags |= NR_SIGNAL;
      cursor += sizeof("signal") - 1;
    } else if (!strncmp(cursor, "apicall", sizeof("apicall") - 1)) {
      event_flags |= NR_APICALL;
      cursor += sizeof("apicall") - 1;
    } else {
      fprintf(stderr, "Unrecognised argument for nodereport events option: %s\n", cursor);
      return 0;
    }
    if (*cursor == '+') {
      cursor++;  // Hop over the '+' separator
    }
  }
  return event_flags;
}

unsigned int ProcessNodeReportCoreSwitch(const char* args) {
  if (strlen(args) == 0) {
    fprintf(stderr, "Missing argument for nodereport core switch option\n");
  } else {
    // Parse the supplied switch
    if (!strncmp(args, "yes", sizeof("yes") - 1) || !strncmp(args, "true", sizeof("true") - 1)) {
      return 1;
    } else if (!strncmp(args, "no", sizeof("no") - 1) || !strncmp(args, "false", sizeof("false") - 1)) {
      return 0;
    } else {
      fprintf(stderr, "Unrecognised argument for nodereport core switch option: %s\n", args);
    }
  }
  return 1;  // Default is to produce core dumps
}

unsigned int ProcessNodeReportSignal(const char* args) {
#ifdef _WIN32
  return 0; // no-op on Windows
#else
  if (strlen(args) == 0) {
    fprintf(stderr, "Missing argument for nodereport signal option\n");
  } else {
    // Parse the supplied switch
    if (!strncmp(args, "SIGUSR2", sizeof("SIGUSR2") - 1)) {
      return SIGUSR2;
    } else if (!strncmp(args, "SIGQUIT", sizeof("SIGQUIT") - 1)) {
      return SIGQUIT;
    } else {
      fprintf(stderr, "Unrecognised argument for nodereport signal option: %s\n", args);
    }
  }
  return SIGUSR2;  // Default signal is SIGUSR2
#endif
}

void ProcessNodeReportFileName(const char* args) {
  if (strlen(args) == 0) {
    fprintf(stderr, "Missing argument for nodereport filename option\n");
    return;
  }
  if (strlen(args) > NR_MAXNAME) {
    fprintf(stderr, "Supplied nodereport filename too long (max %d characters)\n", NR_MAXNAME);
    return;
  }
  snprintf(report_filename, sizeof(report_filename), "%s", args);
}

void ProcessNodeReportDirectory(const char* args) {
  if (strlen(args) == 0) {
    fprintf(stderr, "Missing argument for nodereport directory option\n");
    return;
  }
  if (strlen(args) > NR_MAXPATH) {
    fprintf(stderr, "Supplied nodereport directory path too long (max %d characters)\n", NR_MAXPATH);
    return;
  }
  snprintf(report_directory, sizeof(report_directory), "%s", args);
}

unsigned int ProcessNodeReportVerboseSwitch(const char* args) {
  if (strlen(args) == 0) {
    fprintf(stderr, "Missing argument for nodereport verbose switch option\n");
    return 0;
  }
  // Parse the supplied switch
  if (!strncmp(args, "yes", sizeof("yes") - 1) || !strncmp(args, "true", sizeof("true") - 1)) {
    return 1;
  } else if (!strncmp(args, "no", sizeof("no") - 1) || !strncmp(args, "false", sizeof("false") - 1)) {
    return 0;
  } else {
    fprintf(stderr, "Unrecognised argument for nodereport verbose switch option: %s\n", args);
  }
  return 0;  // Default is verbose mode off
}

/*******************************************************************************
 * Function to save the nodereport module load time value
 *******************************************************************************/
void SetLoadTime() {
#ifdef _WIN32
  GetLocalTime(&loadtime_tm_struct);
#else  // UNIX, OSX
  struct timeval time_val;
  gettimeofday(&time_val, nullptr);
  localtime_r(&time_val.tv_sec, &loadtime_tm_struct);
#endif
}
/*******************************************************************************
 * Main API function to write a NodeReport to file.
 *
 * Parameters:
 *   Isolate* isolate
 *   DumpEvent event
 *   const char* message
 *   const char* location
 *   char* name - in/out - returns the NodeReport filename
 ******************************************************************************/
void TriggerNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, char* name) {
  // Recursion check for NodeReport in progress, bail out
  if (report_active) return; 
  report_active = true;

  // Obtain the current time and the pid (platform dependent)
#ifdef _WIN32
  SYSTEMTIME tm_struct;
  GetLocalTime(&tm_struct);
  DWORD pid = GetCurrentProcessId();
#else  // UNIX, OSX
  struct timeval time_val;
  struct tm tm_struct;
  gettimeofday(&time_val, nullptr);
  localtime_r(&time_val.tv_sec, &tm_struct);
  pid_t pid = getpid();
#endif

  // Determine the required NodeReport filename. In order of priority:
  //   1) supplied on API 2) configured on startup 3) default generated
  char filename[NR_MAXNAME + 1] = "";
  if (name != nullptr && strlen(name) > 0) {
    // Filename was specified as API parameter, use that
    snprintf(filename, sizeof(filename), "%s", name);
  } else if (strlen(report_filename) > 0) {
    // File name was supplied via start-up option, use that
    snprintf(filename, sizeof(filename), "%s", report_filename);
  } else {
    // Construct the NodeReport filename, with timestamp, pid and sequence number
    snprintf(filename, sizeof(filename), "%s", "NodeReport");
    seq++;
#ifdef _WIN32
    snprintf(&filename[strlen(filename)], sizeof(filename) - strlen(filename),
             ".%4d%02d%02d.%02d%02d%02d.%d.%03d.txt",
             tm_struct.wYear, tm_struct.wMonth, tm_struct.wDay,
             tm_struct.wHour, tm_struct.wMinute, tm_struct.wSecond,
             pid, seq);
#else  // UNIX, OSX
    snprintf(&filename[strlen(filename)], sizeof(filename) - strlen(filename),
             ".%4d%02d%02d.%02d%02d%02d.%d.%03d.txt",
             tm_struct.tm_year+1900, tm_struct.tm_mon+1, tm_struct.tm_mday,
             tm_struct.tm_hour, tm_struct.tm_min, tm_struct.tm_sec,
             pid, seq);
#endif
  }

  // Open the NodeReport file stream for writing. Supports stdout/err, user-specified or (default) generated name
  FILE* fp = nullptr;
  if (!strncmp(filename, "stdout", sizeof("stdout") - 1)) {
    fp = stdout;
  } else if (!strncmp(filename, "stderr", sizeof("stderr") - 1)) {
    fp = stderr;
  } else {
    // Regular file. Append filename to directory path if one was specified
    if (strlen(report_directory) > 0) {
      char pathname[NR_MAXPATH + NR_MAXNAME + 1] = "";
#ifdef _WIN32
      snprintf(pathname, sizeof(pathname), "%s%s%s", report_directory, "\\", filename);
#else
      snprintf(pathname, sizeof(pathname), "%s%s%s", report_directory, "/", filename);
#endif
      fp = fopen(pathname, "w");
    } else {
      fp = fopen(filename, "w");
    }
    // Check for errors on the file open
    if (fp == nullptr) {
      if (strlen(report_directory) > 0) {
        fprintf(stderr, "\nFailed to open Node.js report file: %s directory: %s (errno: %d)\n", filename, report_directory, errno);
      } else {
        fprintf(stderr, "\nFailed to open Node.js report file: %s (errno: %d)\n", filename, errno);
      }
      return;
    } else {
      fprintf(stderr, "\nWriting Node.js report to file: %s\n", filename);
    }
  }

  // File stream opened OK, now start printing the NodeReport content, starting with the title
  // and header information (event, filename, timestamp and pid)
  fprintf(fp, "================================================================================\n");
  fprintf(fp, "==== NodeReport ================================================================\n");
  fprintf(fp, "\nEvent: %s, location: \"%s\"\n", message, location);
  fprintf(fp, "Filename: %s\n", filename);

  // Print dump event and module load date/time stamps
#ifdef _WIN32
  fprintf(fp, "Dump event time:  %4d/%02d/%02d %02d:%02d:%02d\n",
          tm_struct.wYear, tm_struct.wMonth, tm_struct.wDay,
          tm_struct.wHour, tm_struct.wMinute, tm_struct.wSecond);
  fprintf(fp, "Module load time: %4d/%02d/%02d %02d:%02d:%02d\n",
          loadtime_tm_struct.wYear, loadtime_tm_struct.wMonth, loadtime_tm_struct.wDay,
          loadtime_tm_struct.wHour, loadtime_tm_struct.wMinute, loadtime_tm_struct.wSecond);
#else  // UNIX, OSX
  fprintf(fp, "Dump event time:  %4d/%02d/%02d %02d:%02d:%02d\n",
          tm_struct.tm_year+1900, tm_struct.tm_mon+1, tm_struct.tm_mday,
          tm_struct.tm_hour, tm_struct.tm_min, tm_struct.tm_sec);
  fprintf(fp, "Module load time: %4d/%02d/%02d %02d:%02d:%02d\n",
          loadtime_tm_struct.tm_year+1900, loadtime_tm_struct.tm_mon+1, loadtime_tm_struct.tm_mday,
          loadtime_tm_struct.tm_hour, loadtime_tm_struct.tm_min, loadtime_tm_struct.tm_sec);
#endif
  // Print native process ID
  fprintf(fp, "Process ID: %d\n", pid);
  fflush(fp);

  // Print Node.js and OS version information
  PrintVersionInformation(fp);
  fflush(fp);

// Print summary JavaScript stack backtrace
  PrintJavaScriptStack(fp, isolate, event, location);
  fflush(fp);

  // Print native stack backtrace
  PrintNativeStack(fp);
  fflush(fp);

  // Print V8 Heap and Garbage Collector information
  PrintGCStatistics(fp, isolate);
  fflush(fp);

  // Print OS and current thread resource usage
#ifndef _WIN32
  PrintResourceUsage(fp);
  fflush(fp);
#endif

  // Print libuv handle summary (TODO: investigate failure on Windows)
  // Note: documentation of the uv_print_all_handles() API says "This function
  // is meant for ad hoc debugging, there is no API/ABI stability guarantee"
  // http://docs.libuv.org/en/v1.x/misc.html
#ifndef _WIN32
  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== Node.js libuv Handle Summary ==============================================\n");
  fprintf(fp, "\n(Flags: R=Ref, A=Active, I=Internal)\n");
  fprintf(fp, "\nFlags Type     Address\n");
  uv_print_all_handles(nullptr, fp);
  fflush(fp);
#endif

  // Print operating system information
  PrintSystemInformation(fp, isolate);

  fprintf(fp, "\n================================================================================\n");
  fflush(fp);
  fclose(fp);

  fprintf(stderr, "Node.js report completed\n");
  if (name != nullptr) {
    snprintf(name, NR_MAXNAME + 1, "%s", filename);  // return the NodeReport file name
  }
  report_active = false;
}

/*******************************************************************************
 * Function to print Node.js version, OS version and machine information
 *
 ******************************************************************************/
static void PrintVersionInformation(FILE* fp) {

  // Print Node.js and deps component versions
  fprintf(fp, "\nNode.js version: %s\n", NODE_VERSION);
  fprintf(fp, "(v8: %s, libuv: %s, zlib: %s, ares: %s)\n",
          V8::GetVersion(), uv_version_string(), ZLIB_VERSION, ARES_VERSION_STR);

  // Print operating system and machine information (Windows)
#ifdef _WIN32
  fprintf(fp, "\nOS version: Windows ");
#if defined(_MSC_VER) && (_MSC_VER >= 1900)
  if (IsWindows1OrGreater()) {
    fprintf(fp, "10 ");
  } else
#endif
  if (IsWindows8OrGreater()) {
    fprintf(fp, "8 ");
  } else if (IsWindows7OrGreater()) {
    fprintf(fp, "7 ");
  } else if (IsWindowsXPOrGreater()) {
    fprintf(fp, "XP ");
  }
  if (IsWindowsServer()) {
    fprintf(fp, "Server\n");
  } else {
    fprintf(fp, "Client\n");
  }
  TCHAR machine_name[256];
  DWORD machine_name_size = 256;
  if (GetComputerName(machine_name, &machine_name_size)) {
    fprintf(fp, "\nMachine: %s %s\n", machine_name);
  }
#else
  // Print operating system and machine information (Unix/OSX)
  struct utsname os_info;
  if (uname(&os_info) == 0) {
    fprintf(fp, "\nOS version: %s %s %s\n", os_info.sysname, os_info.release, os_info.version);
#if defined(__GLIBC__)
    fprintf(fp, "(glibc: %d.%d)\n", __GLIBC__, __GLIBC_MINOR__);
#endif
    fprintf(fp, "\nMachine: %s %s\n", os_info.nodename, os_info.machine);
  }
#endif
}

/*******************************************************************************
 * Function to print the JavaScript stack, if available
 *
 ******************************************************************************/
static void PrintJavaScriptStack(FILE* fp, Isolate* isolate, DumpEvent event, const char* location) {
  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== JavaScript Stack Trace ====================================================\n\n");

#ifdef _WIN32
  switch (event) {
  case kFatalError:
    // Stack trace on fatal error not supported on Windows
    fprintf(fp, "No stack trace available\n");
    break;
  default:
    // All other events, print the stack using StackTrace::StackTrace() and GetStackSample() APIs
    PrintStackFromStackTrace(fp, isolate, event);
    break;
  }  // end switch(event)
#else  // Unix, OSX
  switch (event) {
  case kException:
  case kJavaScript:
    // Print the stack using Message::PrintCurrentStackTrace() API
    Message::PrintCurrentStackTrace(isolate, fp);
    break;
  case kFatalError:
#if NODE_VERSION_AT_LEAST(6, 0, 0)
    if (!strncmp(location, "MarkCompactCollector", sizeof("MarkCompactCollector") - 1)) {
      fprintf(fp, "V8 running in GC - no stack trace available\n");
    } else {
      Message::PrintCurrentStackTrace(isolate, fp);
    }
#else
    fprintf(fp, "No stack trace available\n");
#endif
    break;
  case kSignal_JS:
  case kSignal_UV:
    // Print the stack using StackTrace::StackTrace() and GetStackSample() APIs
    PrintStackFromStackTrace(fp, isolate, event);
    break;
  }  // end switch(event)
#endif
}

/*******************************************************************************
 * Function to print stack using GetStackSample() and StackTrace::StackTrace()
 *
 ******************************************************************************/
static void PrintStackFromStackTrace(FILE* fp, Isolate* isolate, DumpEvent event) {
  v8::RegisterState state;
  v8::SampleInfo info;
  void* samples[255];

  // Initialise the register state
  state.pc = nullptr;
  state.fp = &state;
  state.sp = &state;

  isolate->GetStackSample(state, samples, arraysize(samples), &info);
  if (static_cast<size_t>(info.vm_state) < arraysize(v8_states)) {
    fprintf(fp, "JavaScript VM state: %s\n\n", v8_states[info.vm_state]);
  } else {
    fprintf(fp, "JavaScript VM state: <unknown>\n\n");
  }
  if (event == kSignal_UV) {
    fprintf(fp, "Signal received when event loop idle, no stack trace available\n");
    return;
  }
  Local<StackTrace> stack = StackTrace::CurrentStackTrace(isolate, 255, StackTrace::kDetailed);
  if (stack.IsEmpty()) {
    fprintf(fp, "\nNo stack trace available from StackTrace::CurrentStackTrace()\n");
    return;
  }
  // Print the stack trace, adding in the pc values from GetStackSample() if available
  for (int i = 0; i < stack->GetFrameCount(); i++) {
    if (static_cast<size_t>(i) < info.frames_count) {
      PrintStackFrame(fp, isolate, stack->GetFrame(i), i, samples[i]);
    } else {
      PrintStackFrame(fp, isolate, stack->GetFrame(i), i, nullptr);
    }
  }
}

/*******************************************************************************
 * Function to print a JavaScript stack frame from a V8 StackFrame object
 *
 ******************************************************************************/
static void PrintStackFrame(FILE* fp, Isolate* isolate, Local<StackFrame> frame, int i, void* pc) {
  Nan::Utf8String fn_name_s(frame->GetFunctionName());
  Nan::Utf8String script_name(frame->GetScriptName());
  const int line_number = frame->GetLineNumber();
  const int column = frame->GetColumn();

  // First print the frame index and the instruction address
#ifdef _WIN32
  fprintf(fp, "%2d: [pc=0x%p] ", i, pc);
#else
  fprintf(fp, "%2d: [pc=%p] ", i, pc);
#endif

  // Now print the JavaScript function name and source information
  if (frame->IsEval()) {
    if (frame->GetScriptId() == Message::kNoScriptIdInfo) {
      fprintf(fp, "at [eval]:%i:%i\n", line_number, column);
    } else {
      fprintf(fp, "at [eval] (%s:%i:%i)\n", *script_name, line_number, column);
    }
    return;
  }

  if (fn_name_s.length() == 0) {
    fprintf(fp, "%s:%i:%i\n", *script_name, line_number, column);
  } else {
    if (frame->IsConstructor()) {
      fprintf(fp, "%s [constructor] (%s:%i:%i)\n", *fn_name_s, *script_name, line_number, column);
    } else {
      fprintf(fp, "%s (%s:%i:%i)\n", *fn_name_s, *script_name, line_number, column);
    }
  }
}


#ifdef _WIN32
/*******************************************************************************
 * Function to print a native stack backtrace
 *
 ******************************************************************************/
void PrintNativeStack(FILE* fp) {
  void* frames[64];
  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== Native Stack Trace ========================================================\n\n");

  HANDLE hProcess = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(hProcess, nullptr, TRUE);

  WORD numberOfFrames = CaptureStackBackTrace(2, 64, frames, nullptr);
  
  // Walk the frames printing symbolic information if available
  for (int i = 0; i < numberOfFrames; i++) {
    DWORD64 dwOffset64 = 0;
    DWORD64 dwAddress = reinterpret_cast<DWORD64>(frames[i]);
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO pSymbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    if (SymFromAddr(hProcess, dwAddress, &dwOffset64, pSymbol)) {
        DWORD dwOffset = 0;
        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        if (SymGetLineFromAddr64(hProcess, dwAddress, &dwOffset, &line)) {
          fprintf(fp, "%2d: [pc=0x%p] %s [+%d] in %s: line: %lu\n", i, pSymbol->Address, pSymbol->Name, dwOffset, line.FileName, line.LineNumber);
        } else {
          // SymGetLineFromAddr64() failed, just print the address and symbol
          if (dwOffset64 <= 32) { // sanity check
            fprintf(fp, "%2d: [pc=0x%p] %s [+%d]\n", i, pSymbol->Address, pSymbol->Name, dwOffset64);
          } else {
            fprintf(fp, "%2d: [pc=0x%p]\n", i, pSymbol->Address);
          }
        }
    } else { // SymFromAddr() failed, just print the address
      fprintf(fp, "%2d: [pc=0x%p]\n", i, pSymbol->Address);
    }
  }
}
#else
/*******************************************************************************
 * Function to print a native stack backtrace - Linux/OSX
 *
 ******************************************************************************/
void PrintNativeStack(FILE* fp) {
  void* frames[256];
  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== Native Stack Trace ========================================================\n\n");

  // Get the native backtrace (array of instruction addresses)
  const int size = backtrace(frames, arraysize(frames));
  if (size <= 0) {
    fprintf(fp, "Native backtrace failed, error %d\n", size);
    return;
  } else if (size <=2) {
    fprintf(fp, "No frames to print\n");
    return;
  }

  // Print the native frames, omitting the top 3 frames as they are in nodereport code
  // backtrace_symbols_fd(frames, size, fileno(fp));
  for (int i = 2; i < size; i++) {
    // print frame index and instruction address
    fprintf(fp, "%2d: [pc=%p] ", i-2, frames[i]);
    // If we can translate the address using dladdr() print additional symbolic information
    Dl_info info;
    if (dladdr(frames[i], &info)) {
      if (info.dli_sname != nullptr) {
        if (char* demangled = abi::__cxa_demangle(info.dli_sname, 0, 0, 0)) {
          fprintf(fp, "%s", demangled); // print demangled symbol name
          free(demangled);
        } else {
          fprintf(fp, "%s", info.dli_sname); // just print the symbol name
        }
      }
      if (info.dli_fname != nullptr) {
        fprintf(fp, " [%s]", info.dli_fname); // print shared object name
      }
    }
    fprintf(fp, "\n");
  }
}
#endif

/*******************************************************************************
 * Function to print V8 JavaScript heap information.
 *
 * This uses the existing V8 HeapStatistics and HeapSpaceStatistics APIs.
 * The isolate->GetGCStatistics(&heap_stats) internal V8 API could potentially
 * provide some more useful information - the GC history and the handle counts
 ******************************************************************************/
static void PrintGCStatistics(FILE* fp, Isolate* isolate) {
  HeapStatistics v8_heap_stats;
  isolate->GetHeapStatistics(&v8_heap_stats);

  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== JavaScript Heap and Garbage Collector =====================================\n");
  HeapSpaceStatistics v8_heap_space_stats;
  // Loop through heap spaces
  for (size_t i = 0; i < isolate->NumberOfHeapSpaces(); i++) {
    isolate->GetHeapSpaceStatistics(&v8_heap_space_stats, i);
    fprintf(fp, "\nHeap space name: %s", v8_heap_space_stats.space_name());
    fprintf(fp, "\n    Memory size: ");
    WriteInteger(fp, v8_heap_space_stats.space_size());
    fprintf(fp, " bytes, committed memory: ");
    WriteInteger(fp, v8_heap_space_stats.physical_space_size());
    fprintf(fp, " bytes\n    Capacity: ");
    WriteInteger(fp, v8_heap_space_stats.space_used_size() +
                           v8_heap_space_stats.space_available_size());
    fprintf(fp, " bytes, used: ");
    WriteInteger(fp, v8_heap_space_stats.space_used_size());
    fprintf(fp, " bytes, available: ");
    WriteInteger(fp, v8_heap_space_stats.space_available_size());
    fprintf(fp, " bytes");
  }

  fprintf(fp, "\n\nTotal heap memory size: ");
  WriteInteger(fp, v8_heap_stats.total_heap_size());
  fprintf(fp, " bytes\nTotal heap committed memory: ");
  WriteInteger(fp, v8_heap_stats.total_physical_size());
  fprintf(fp, " bytes\nTotal used heap memory: ");
  WriteInteger(fp, v8_heap_stats.used_heap_size());
  fprintf(fp, " bytes\nTotal available heap memory: ");
  WriteInteger(fp, v8_heap_stats.total_available_size());
  fprintf(fp, " bytes\n\nHeap memory limit: ");
  WriteInteger(fp, v8_heap_stats.heap_size_limit());
  fprintf(fp, "\n");
}

#ifndef _WIN32
/*******************************************************************************
 * Function to print resource usage (Linux/OSX only).
 *
 ******************************************************************************/
static void PrintResourceUsage(FILE* fp) {
  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== Resource Usage ============================================================\n");

  // Process and current thread usage statistics
  struct rusage stats;
  fprintf(fp, "\nProcess total resource usage:");
  if (getrusage(RUSAGE_SELF, &stats) == 0) {
#ifdef __APPLE__
    fprintf(fp, "\n  User mode CPU: %ld.%06d secs", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    fprintf(fp, "\n  Kernel mode CPU: %ld.%06d secs", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
#else
    fprintf(fp, "\n  User mode CPU: %ld.%06ld secs", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    fprintf(fp, "\n  Kernel mode CPU: %ld.%06ld secs", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
#endif
    fprintf(fp, "\n  Maximum resident set size: ");
    WriteInteger(fp, stats.ru_maxrss * 1024);
    fprintf(fp, " bytes\n  Page faults: %ld (I/O required) %ld (no I/O required)", stats.ru_majflt, stats.ru_minflt);
    fprintf(fp, "\n  Filesystem activity: %ld reads %ld writes", stats.ru_inblock, stats.ru_oublock);
  }
#ifdef RUSAGE_THREAD
  fprintf(fp, "\n\nEvent loop thread resource usage:");
  if (getrusage(RUSAGE_THREAD, &stats) == 0) {
    fprintf(fp, "\n  User mode CPU: %ld.%06ld secs", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    fprintf(fp, "\n  Kernel mode CPU: %ld.%06ld secs", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
    fprintf(fp, "\n  Filesystem activity: %ld reads %ld writes", stats.ru_inblock, stats.ru_oublock);
  }
#endif
  fprintf(fp, "\n");
}
#endif

/*******************************************************************************
 * Function to print operating system information.
 *
 ******************************************************************************/
static void PrintSystemInformation(FILE* fp, Isolate* isolate) {
  fprintf(fp, "\n================================================================================");
  fprintf(fp, "\n==== System Information ========================================================\n");

#ifdef _WIN32
  fprintf(fp, "\nEnvironment variables\n");
  LPTSTR lpszVariable;
  LPTCH lpvEnv;

  // Get pointer to the environment block
  lpvEnv = GetEnvironmentStrings();
  if (lpvEnv != nullptr) {
    // Variable strings are separated by null bytes, and the block is terminated by a null byte.
    lpszVariable = reinterpret_cast<LPTSTR>(lpvEnv);
    while (*lpszVariable) {
      fprintf(fp, "  %s\n", lpszVariable);
      lpszVariable += lstrlen(lpszVariable) + 1;
    }
    FreeEnvironmentStrings(lpvEnv);
  }
#else
  fprintf(fp, "\nEnvironment variables\n");
  int index = 1;
  char* env_var = *environ;

  while (env_var != nullptr) {
    fprintf(fp, "  %s\n", env_var);
    env_var = *(environ + index++);
  }

const static struct {
  const char* description;
  int id;
} rlimit_strings[] = {
  {"core file size (blocks)       ", RLIMIT_CORE},
  {"data seg size (kbytes)        ", RLIMIT_DATA},
  {"file size (blocks)            ", RLIMIT_FSIZE},
  {"max locked memory (bytes)     ", RLIMIT_MEMLOCK},
  {"max memory size (kbytes)      ", RLIMIT_RSS},
  {"open files                    ", RLIMIT_NOFILE},
  {"stack size (bytes)            ", RLIMIT_STACK},
  {"cpu time (seconds)            ", RLIMIT_CPU},
  {"max user processes            ", RLIMIT_NPROC},
  {"virtual memory (kbytes)       ", RLIMIT_AS}
};

  fprintf(fp, "\nResource limits                        soft limit      hard limit\n");
  struct rlimit limit;

  for (size_t i = 0; i < arraysize(rlimit_strings); i++) {
    if (getrlimit(rlimit_strings[i].id, &limit) == 0) {
      fprintf(fp, "  %s ", rlimit_strings[i].description);
      if (limit.rlim_cur == RLIM_INFINITY) {
        fprintf(fp, "       unlimited");
      } else {
        fprintf(fp, "%16" PRIu64, limit.rlim_cur);
      }
      if (limit.rlim_max == RLIM_INFINITY) {
        fprintf(fp, "       unlimited\n");
      } else {
        fprintf(fp, "%16" PRIu64 "\n", limit.rlim_max);
      }
    }
  }
#endif
}

/*******************************************************************************
 * Utility function to print out integer values with commas for readability.
 *
 ******************************************************************************/
static void WriteInteger(FILE* fp, size_t value) {
  int thousandsStack[8];  // Sufficient for max 64-bit number
  int stackTop = 0;
  int i;
  size_t workingValue = value;

  do {
    thousandsStack[stackTop++] = workingValue % 1000;
    workingValue /= 1000;
  } while (workingValue != 0);

  for (i = stackTop-1; i >= 0; i--) {
    if (i == (stackTop-1)) {
      fprintf(fp, "%u", thousandsStack[i]);
    } else {
      fprintf(fp, "%03u", thousandsStack[i]);
    }
    if (i > 0) {
       fprintf(fp, ",");
    }
  }
}

}  // namespace nodereport
