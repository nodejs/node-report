#include "node_report.h"
#include "v8.h"
#include "uv.h"
#include "time.h"

#include <fcntl.h>
#include <map>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>

#if !defined(_MSC_VER)
#include <strings.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#include <process.h>
#include <dbghelp.h>
#include <Lm.h>
#include <tchar.h>
#include <psapi.h>
#else
#include <sys/time.h>
#include <sys/resource.h>
// Get the standard printf format macros for C99 stdint types.
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <inttypes.h>
#include <cxxabi.h>
#include <dlfcn.h>
#if defined(__linux__) || defined(__sun)
#include <link.h>
#endif
#ifdef __sun
#include <procfs.h>
#endif
#ifdef _AIX
#include <sys/ldr.h>
#include <sys/procfs.h>
#endif
#ifndef _AIX
#include <execinfo.h>
#endif
#include <sys/utsname.h>
#endif

#if !defined(NODEREPORT_VERSION)
#define NODEREPORT_VERSION "dev"
#endif

#if !defined(UNKNOWN_NODEVERSION_STRING)
#define UNKNOWN_NODEVERSION_STRING "Unable to determine Node.js version\n"
#endif

#ifdef __APPLE__
// Include _NSGetArgv and _NSGetArgc for command line arguments.
#include <crt_externs.h>
#include <mach-o/dyld.h>
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
using v8::String;
using v8::V8;

#ifdef _WIN32
typedef SYSTEMTIME TIME_TYPE;
#else  // UNIX, OSX
typedef struct tm TIME_TYPE;
#endif

// Internal/static function declarations
static void WriteNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, char* filename, std::ostream &out, MaybeLocal<Value> error, TIME_TYPE* time);
static void PrintCommandLine(std::ostream& out);
static void PrintVersionInformation(std::ostream& out);
static void PrintJavaScriptStack(std::ostream& out, Isolate* isolate, DumpEvent event, const char* location);
static void PrintJavaScriptErrorStack(std::ostream& out, Isolate* isolate, MaybeLocal<Value> error);
static void PrintStackFromStackTrace(std::ostream& out, Isolate* isolate, DumpEvent event);
static void PrintStackFrame(std::ostream& out, Isolate* isolate, Local<StackFrame> frame, int index, void* pc);
static void PrintNativeStack(std::ostream& out);
#ifndef _WIN32
static void PrintResourceUsage(std::ostream& out);
#endif
static void PrintGCStatistics(std::ostream& out, Isolate* isolate);
static void PrintSystemInformation(std::ostream& out, Isolate* isolate);
static void PrintLoadedLibraries(std::ostream& out, Isolate* isolate);
static void WriteInteger(std::ostream& out, size_t value);

// Global variables
static int seq = 0;  // sequence number for report filenames
const char* v8_states[] = {"JS", "GC", "COMPILER", "OTHER", "EXTERNAL", "IDLE"};
static bool report_active = false; // recursion protection
static char report_filename[NR_MAXNAME + 1] = "";
static char report_directory[NR_MAXPATH + 1] = ""; // defaults to current working directory
std::string version_string = UNKNOWN_NODEVERSION_STRING;
std::string commandline_string = "";
static TIME_TYPE loadtime_tm_struct; // module load time
static time_t load_time; // module load time absolute

/*******************************************************************************
 * Functions to process node-report configuration options:
 *   Trigger event selection
 *   Core dump yes/no switch
 *   Trigger signal selection
 *   Report filename
 *   Report directory
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
      std::cerr << "Unrecognised argument for node-report events option: " << cursor << "\n";
      return 0;
    }
    if (*cursor == '+') {
      cursor++;  // Hop over the '+' separator
    }
  }
  return event_flags;
}

unsigned int ProcessNodeReportSignal(const char* args) {
#ifdef _WIN32
  return 0; // no-op on Windows
#else
  if (strlen(args) == 0) {
    std::cerr << "Missing argument for node-report signal option\n";
  } else {
    // Parse the supplied switch
    if (!strncmp(args, "SIGUSR2", sizeof("SIGUSR2") - 1)) {
      return SIGUSR2;
    } else if (!strncmp(args, "SIGQUIT", sizeof("SIGQUIT") - 1)) {
      return SIGQUIT;
    } else {
     std::cerr << "Unrecognised argument for node-report signal option: "<< args << "\n";
    }
  }
  return SIGUSR2;  // Default signal is SIGUSR2
#endif
}

void ProcessNodeReportFileName(const char* args) {
  if (strlen(args) == 0) {
    std::cerr << "Missing argument for node-report filename option\n";
    return;
  }
  if (strlen(args) > NR_MAXNAME) {
    std::cerr << "Supplied node-report filename too long (max " << NR_MAXNAME << " characters)\n";
    return;
  }
  snprintf(report_filename, sizeof(report_filename), "%s", args);
}

void ProcessNodeReportDirectory(const char* args) {
  if (strlen(args) == 0) {
    std::cerr << "Missing argument for node-report directory option\n";
    return;
  }
  if (strlen(args) > NR_MAXPATH) {
    std::cerr << "Supplied node-report directory path too long (max " << NR_MAXPATH << " characters)\n";
    return;
  }
  snprintf(report_directory, sizeof(report_directory), "%s", args);
}

unsigned int ProcessNodeReportVerboseSwitch(const char* args) {
  if (strlen(args) == 0) {
    std::cerr << "Missing argument for node-report verbose switch option\n";
    return 0;
  }
  // Parse the supplied switch
  if (!strncmp(args, "yes", sizeof("yes") - 1) || !strncmp(args, "true", sizeof("true") - 1)) {
    return 1;
  } else if (!strncmp(args, "no", sizeof("no") - 1) || !strncmp(args, "false", sizeof("false") - 1)) {
    return 0;
  } else {
    std::cerr << "Unrecognised argument for node-report verbose switch option: " << args << "\n";
  }
  return 0;  // Default is verbose mode off
}

void SetVersionString(Isolate* isolate) {
  // Catch anything thrown and gracefully return
  Nan::TryCatch trycatch;
  version_string = UNKNOWN_NODEVERSION_STRING;

  // Retrieve the process object
  v8::Local<v8::String> process_prop;
  if (!Nan::New<v8::String>("process").ToLocal(&process_prop)) return;
  v8::Local<v8::Object> global_obj = isolate->GetCurrentContext()->Global();
  v8::Local<v8::Value> process_value;
  if (!Nan::Get(global_obj, process_prop).ToLocal(&process_value)) return;
  if (!process_value->IsObject()) return;
  v8::Local<v8::Object> process_obj = process_value.As<v8::Object>();

  // Get process.version
  v8::Local<v8::String> version_prop;
  if (!Nan::New<v8::String>("version").ToLocal(&version_prop)) return;
  v8::Local<v8::Value> version;
  if (!Nan::Get(process_obj, version_prop).ToLocal(&version)) return;

  // e.g. Node.js version: v6.9.1
  if (version->IsString()) {
    Nan::Utf8String node_version(version);
    version_string = "Node.js version: ";
    version_string += *node_version;
    version_string += "\n";
  }

  // Get process.versions
  v8::Local<v8::String> versions_prop;
  if (!Nan::New<v8::String>("versions").ToLocal(&versions_prop)) return;
  v8::Local<v8::Value> versions_value;
  if (!Nan::Get(process_obj, versions_prop).ToLocal(&versions_value)) return;
  if (!versions_value->IsObject()) return;
  v8::Local<v8::Object> versions_obj = versions_value.As<v8::Object>();

  // Get component names and versions from process.versions
  v8::Local<v8::Array> components;
  if (!Nan::GetOwnPropertyNames(versions_obj).ToLocal(&components)) return;
  v8::Local<v8::Object> components_obj = components.As<v8::Object>();
  std::map<std::string, std::string> comp_versions;
  uint32_t total_components = (*components)->Length();
  for (uint32_t i = 0; i < total_components; i++) {
    v8::Local<v8::Value> name_value;
    if (!Nan::Get(components_obj, i).ToLocal(&name_value)) continue;
    v8::Local<v8::Value> version_value;
    if (!Nan::Get(versions_obj, name_value).ToLocal(&version_value)) continue;

    Nan::Utf8String component_name(name_value);
    Nan::Utf8String component_version(version_value);
    if (*component_name == nullptr || *component_version == nullptr) continue;

    // Don't duplicate the Node.js version
    if (!strcmp("node", *component_name)) {
      if (version_string == UNKNOWN_NODEVERSION_STRING) {
        version_string = "Node.js version: v";
        version_string += *component_version;
        version_string += "\n";
      }
    } else {
      comp_versions[*component_name] = *component_version;
    }
  }

  // Format sorted component versions surrounded by (), wrapped
  // e.g.
  // (ares: 1.10.1-DEV, http_parser: 2.7.0, icu: 57.1, modules: 48,
  //  openssl: 1.0.2j, uv: 1.9.1, v8: 5.1.281.84, zlib: 1.2.8)
  const size_t wrap = 80;
  version_string += "(";
  const char* separator = "";
  std::string versions = "";
  for (auto it : comp_versions) {
    std::string comp_version_string = it.first;
    comp_version_string += ": ";
    comp_version_string += it.second;
    versions += separator;
    if (wrap - (versions.length() % wrap) < comp_version_string.length()) {
      versions += "\n ";
    }
    separator = ", ";
    versions += comp_version_string;
  }
  version_string += versions + ")\n";
}

/*******************************************************************************
 * Function to save the node-report module load time value
 *******************************************************************************/
void SetLoadTime() {
#ifdef _WIN32
  GetLocalTime(&loadtime_tm_struct);
#else  // UNIX, OSX
  struct timeval time_val;
  gettimeofday(&time_val, nullptr);
  localtime_r(&time_val.tv_sec, &loadtime_tm_struct);
#endif
  time(&load_time);
}

/*******************************************************************************
 * Function to save the process command line
 *******************************************************************************/
void SetCommandLine() {
#ifdef __linux__
  // Read the command line from /proc/self/cmdline
  char buf[64];
  FILE* cmdline_fd = fopen("/proc/self/cmdline", "r");
  if (cmdline_fd == nullptr) {
    return;
  }
  commandline_string = "";
  int bytesread = fread(buf, 1, sizeof(buf), cmdline_fd);
  while (bytesread > 0) {
    for (int i = 0; i < bytesread; i++) {
      // Arguments are null separated.
      if (buf[i] == '\0') {
        commandline_string += " ";
      } else {
        commandline_string += buf[i];
      }
    }
    bytesread = fread(buf, 1, sizeof(buf), cmdline_fd);
  }
  fclose(cmdline_fd);
#elif __APPLE__
  char **argv = *_NSGetArgv();
  int argc = *_NSGetArgc();

  commandline_string = "";
  std::string separator = "";
  for (int i = 0; i < argc; i++) {
    commandline_string += separator + argv[i];
    separator = " ";
  }
#elif defined(_AIX) || defined(__sun)
  // Read the command line from /proc/self/cmdline
  char procbuf[64];
  snprintf(procbuf, sizeof(procbuf), "/proc/%d/psinfo", getpid());
  FILE* psinfo_fd = fopen(procbuf, "r");
  if (psinfo_fd == nullptr) {
    return;
  }
  psinfo_t info;
  int bytesread = fread(&info, 1, sizeof(psinfo_t), psinfo_fd);
  fclose(psinfo_fd);
  if (bytesread == sizeof(psinfo_t)) {
    commandline_string = "";
    std::string separator = "";
#ifdef _AIX
    char **argv = *((char ***) info.pr_argv);
#else
    char **argv = ((char **) info.pr_argv);
#endif
    for (auto i = 0; i < info.pr_argc && argv[i] != nullptr; i++) {
      commandline_string += separator + argv[i];
      separator = " ";
    }
  }
#elif _WIN32
  commandline_string = GetCommandLine();
#endif
}

/*******************************************************************************
 * Main API function to write a report to file.
 *
 * Parameters:
 *   Isolate* isolate
 *   DumpEvent event
 *   const char* message
 *   const char* location
 *   char* name - in/out - returns the report filename
 *   MaybeLocal<Value> error - JavaScript Error object.
 ******************************************************************************/
void TriggerNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, char* name, MaybeLocal<Value> error) {
  // Recursion check for report in progress, bail out
  if (report_active) return;
  report_active = true;

  // Obtain the current time and the pid (platform dependent)
  TIME_TYPE tm_struct;
#ifdef _WIN32
  GetLocalTime(&tm_struct);
  DWORD pid = GetCurrentProcessId();
#else  // UNIX, OSX
  struct timeval time_val;
  gettimeofday(&time_val, nullptr);
  localtime_r(&time_val.tv_sec, &tm_struct);
  pid_t pid = getpid();
#endif

  // Determine the required report filename. In order of priority:
  //   1) supplied on API 2) configured on startup 3) default generated
  char filename[NR_MAXNAME + 1] = "";
  if (name != nullptr && strlen(name) > 0) {
    // Filename was specified as API parameter, use that
    snprintf(filename, sizeof(filename), "%s", name);
  } else if (strlen(report_filename) > 0) {
    // File name was supplied via start-up option, use that
    snprintf(filename, sizeof(filename), "%s", report_filename);
  } else {
    // Construct the report filename, with timestamp, pid and sequence number
    snprintf(filename, sizeof(filename), "%s", "node-report");
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

  // Open the report file stream for writing. Supports stdout/err, user-specified or (default) generated name
  std::ofstream outfile;
  std::ostream* outstream = &std::cout;
  if (!strncmp(filename, "stdout", sizeof("stdout") - 1)) {
    outstream = &std::cout;
  } else if (!strncmp(filename, "stderr", sizeof("stderr") - 1)) {
    outstream = &std::cerr;
  } else {
    // Regular file. Append filename to directory path if one was specified
    if (strlen(report_directory) > 0) {
      char pathname[NR_MAXPATH + NR_MAXNAME + 1] = "";
#ifdef _WIN32
      snprintf(pathname, sizeof(pathname), "%s%s%s", report_directory, "\\", filename);
#else
      snprintf(pathname, sizeof(pathname), "%s%s%s", report_directory, "/", filename);
#endif
      outfile.open(pathname, std::ios::out);
    } else {
      outfile.open(filename, std::ios::out);
    }
    // Check for errors on the file open
    if (!outfile.is_open()) {
      if (strlen(report_directory) > 0) {
        std::cerr << "\nFailed to open Node.js report file: " << filename << " directory: " << report_directory << " (errno: " << errno << ")\n";
      } else {
        std::cerr << "\nFailed to open Node.js report file: " << filename << " (errno: " << errno << ")\n";
      }
      return;
    } else {
      std::cerr << "\nWriting Node.js report to file: " << filename << "\n";
    }
  }

  // Pass our stream about by reference, not by copying it.
  std::ostream &out = outfile.is_open() ? outfile : *outstream;

  WriteNodeReport(isolate, event, message, location, filename, out, error, &tm_struct);

  // Do not close stdout/stderr, only close files we opened.
  if(outfile.is_open()) {
    outfile.close();
  }

  std::cerr << "Node.js report completed\n";
  if (name != nullptr) {
    snprintf(name, NR_MAXNAME + 1, "%s", filename);  // return the report file name
  }

}

void GetNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, MaybeLocal<Value> error, std::ostream& out) {
  // Obtain the current time and the pid (platform dependent)
  TIME_TYPE tm_struct;
#ifdef _WIN32
  GetLocalTime(&tm_struct);
#else  // UNIX, OSX
  struct timeval time_val;
  gettimeofday(&time_val, nullptr);
  localtime_r(&time_val.tv_sec, &tm_struct);
#endif
  WriteNodeReport(isolate, event, message, location, nullptr, out, error, &tm_struct);
}

static void reportEndpoints(uv_handle_t* h, std::ostringstream& out) {
  struct sockaddr_storage addr_storage;
  struct sockaddr* addr = (sockaddr*)&addr_storage;
  char hostbuf[NI_MAXHOST];
  char portbuf[NI_MAXSERV];
  uv_any_handle* handle = (uv_any_handle*)h;
  int addr_size = sizeof(addr_storage);
  int rc = -1;

  switch (h->type) {
    case UV_UDP: {
      rc = uv_udp_getsockname(&(handle->udp), addr, &addr_size);
      break;
    }
    case UV_TCP: {
      rc = uv_tcp_getsockname(&(handle->tcp), addr, &addr_size);
      break;
    }
    default: break;
  }
  if (rc == 0) {
    // getnameinfo will format host and port and handle IPv4/IPv6.
    rc = getnameinfo(addr, addr_size, hostbuf, sizeof(hostbuf), portbuf,
                     sizeof(portbuf), NI_NUMERICSERV);
    if (rc == 0) {
      out << std::string(hostbuf) << ":" << std::string(portbuf);
    }

    if (h->type == UV_TCP) {
      // Get the remote end of the connection.
      rc = uv_tcp_getpeername(&(handle->tcp), addr, &addr_size);
      if (rc == 0) {
        rc = getnameinfo(addr, addr_size, hostbuf, sizeof(hostbuf), portbuf,
                         sizeof(portbuf), NI_NUMERICSERV);
        if (rc == 0) {
          out << " connected to ";
          out << std::string(hostbuf) << ":" << std::string(portbuf);
        }
      } else if (rc == UV_ENOTCONN) {
        out << " (not connected)";
      }
    }
  }
}

static void reportPath(uv_handle_t* h, std::ostringstream& out) {
  char *buffer = nullptr;
  int rc = -1;
  size_t size = 0;
  uv_any_handle* handle = (uv_any_handle*)h;
  // First call to get required buffer size.
  switch (h->type) {
    case UV_FS_EVENT: {
      rc = uv_fs_event_getpath(&(handle->fs_event), buffer, &size);
      break;
    }
    case UV_FS_POLL: {
      rc = uv_fs_poll_getpath(&(handle->fs_poll), buffer, &size);
      break;
    }
    default: break;
  }
  if (rc == UV_ENOBUFS) {
    buffer = static_cast<char *>(malloc(size));
    switch (h->type) {
      case UV_FS_EVENT: {
        rc = uv_fs_event_getpath(&(handle->fs_event), buffer, &size);
        break;
      }
      case UV_FS_POLL: {
        rc = uv_fs_poll_getpath(&(handle->fs_poll), buffer, &size);
        break;
      }
      default: break;
    }
    if (rc == 0) {
      // buffer is not null terminated.
      std::string name(buffer, size);
      out << "filename: " << name;
    }
    free(buffer);
  }
}

static void walkHandle(uv_handle_t* h, void* arg) {
  std::string type;
  std::ostringstream data;
  std::ostream* out = reinterpret_cast<std::ostream*>(arg);
  uv_any_handle* handle = (uv_any_handle*)h;

  // List all the types so we get a compile warning if we've missed one,
  // (using default: supresses the compiler warning).
  switch (h->type) {
    case UV_UNKNOWN_HANDLE: type = "unknown"; break;
    case UV_ASYNC: type = "async"; break;
    case UV_CHECK: type = "check"; break;
    case UV_FS_EVENT: {
      type = "fs_event";
      reportPath(h, data);
      break;
    }
    case UV_FS_POLL: {
      type = "fs_poll";
      reportPath(h, data);
      break;
    }
    case UV_HANDLE: type = "handle"; break;
    case UV_IDLE: type = "idle"; break;
    case UV_NAMED_PIPE: type = "pipe"; break;
    case UV_POLL: type = "poll"; break;
    case UV_PREPARE: type = "prepare"; break;
    case UV_PROCESS: {
      type = "process";
      data << "pid: " << handle->process.pid;
      break;
    }
    case UV_STREAM: type = "stream"; break;
    case UV_TCP: {
      type = "tcp";
      reportEndpoints(h, data);
      break;
    }
    case UV_TIMER: {
      // TODO timeout/due is not actually public however it is present
      // in all current versions of libuv. Once uv_timer_get_timeout is
      // in a supported level of libuv we should test for it with dlsym
      // and use it instead, in case timeout moves in the future.
#ifdef _WIN32
      uint64_t due = handle->timer.due;
#else
      uint64_t due = handle->timer.timeout;
#endif
      uint64_t now = uv_now(handle->timer.loop);
      type = "timer";
      data << "repeat: " << uv_timer_get_repeat(&(handle->timer));
      if (due > now) {
          data << ", timeout in: " << (due - now) << " ms";
      } else {
          data << ", timeout expired: " << (now - due) << " ms ago";
      }
      break;
    }
    case UV_TTY: {
      int height, width, rc;
      type = "tty";
      rc = uv_tty_get_winsize(&(handle->tty), &width, &height);
      if (rc == 0) {
        data << "width: " << width << ", height: " << height;
      }
      break;
    }
    case UV_UDP: {
      type = "udp";
      reportEndpoints(h, data);
      break;
    }
    case UV_SIGNAL: {
      // SIGWINCH is used by libuv so always appears.
      // See http://docs.libuv.org/en/v1.x/signal.html
      type = "signal";
      data << "signum: " << handle->signal.signum
      // node::signo_string() is not exported by Node.js on Windows.
#ifndef _WIN32
           << " (" << node::signo_string(handle->signal.signum) << ")"
#endif
           ;
      break;
    }
    case UV_FILE: type = "file"; break;
    // We shouldn't see "max" type
    case UV_HANDLE_TYPE_MAX : type = "max"; break;
  }

  if (h->type == UV_TCP || h->type == UV_UDP
#ifndef _WIN32
      || h->type == UV_NAMED_PIPE
#endif
      ) {
    // These *must* be 0 or libuv will set the buffer sizes to the non-zero
    // values they contain.
    int send_size = 0;
    int recv_size = 0;
    if (h->type == UV_TCP || h->type == UV_UDP) {
      data << ", ";
    }
    uv_send_buffer_size(h, &send_size);
    uv_recv_buffer_size(h, &recv_size);
    data << "send buffer size: " << send_size
         << ", recv buffer size: " << recv_size;
  }

  if (h->type == UV_TCP || h->type == UV_NAMED_PIPE || h->type == UV_TTY ||
      h->type == UV_UDP || h->type == UV_POLL) {
    uv_os_fd_t fd_v;
    uv_os_fd_t* fd = &fd_v;
    int rc  = uv_fileno(h, fd);
    // uv_os_fd_t is an int on Unix and HANDLE on Windows.
#ifndef _WIN32
    if (rc == 0) {
      switch (fd_v) {
      case 0:
        data << ", stdin"; break;
      case 1:
        data << ", stdout"; break;
      case 2:
        data << ", stderr"; break;
      default:
        data << ", file descriptor: " << static_cast<int>(fd_v);
        break;
      }
    }
#endif
  }

  if (h->type == UV_TCP || h->type == UV_NAMED_PIPE || h->type == UV_TTY) {

    data << ", write queue size: "
         << handle->stream.write_queue_size;
    data << (uv_is_readable(&handle->stream) ? ", readable" : "")
         << (uv_is_writable(&handle->stream) ? ", writable": "");

  }

  *out << std::left << "[" << (uv_has_ref(h) ? 'R' : '-')
       << (uv_is_active(h) ? 'A' : '-') << "]   " << std::setw(10) << type
       << std::internal << std::setw(2 + 2 * sizeof(void*));
  char prev_fill = out->fill('0');
  *out << static_cast<void*>(h) << std::left;
  out->fill(prev_fill);
  *out << "  " << std::left << data.str() << std::endl;
}

static void WriteNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, char* filename, std::ostream &out, MaybeLocal<Value> error, TIME_TYPE* tm_struct) {

#ifdef _WIN32
  DWORD pid = GetCurrentProcessId();
#else  // UNIX, OSX
  pid_t pid = getpid();
#endif

  // Save formatting for output stream.
  std::ios oldState(nullptr);
  oldState.copyfmt(out);

  // File stream opened OK, now start printing the report content, starting with the title
  // and header information (event, filename, timestamp and pid)
  out << "================================================================================\n";
  out << "==== Node Report ===============================================================\n";
  out << "\nEvent: " << message << ", location: \"" << location << "\"\n";
  if( filename != nullptr ) {
    out << "Filename: " << filename << "\n";
  }

  // Print dump event and module load date/time stamps
  char timebuf[64];
#ifdef _WIN32
  snprintf(timebuf, sizeof(timebuf), "%4d/%02d/%02d %02d:%02d:%02d",
          tm_struct->wYear, tm_struct->wMonth, tm_struct->wDay,
          tm_struct->wHour, tm_struct->wMinute, tm_struct->wSecond);
  out << "Dump event time:  "<< timebuf << "\n";
  snprintf(timebuf, sizeof(timebuf), "%4d/%02d/%02d %02d:%02d:%02d",
          loadtime_tm_struct.wYear, loadtime_tm_struct.wMonth, loadtime_tm_struct.wDay,
          loadtime_tm_struct.wHour, loadtime_tm_struct.wMinute, loadtime_tm_struct.wSecond);
  out << "Module load time: " << timebuf << "\n";
#else  // UNIX, OSX
  snprintf(timebuf, sizeof(timebuf), "%4d/%02d/%02d %02d:%02d:%02d",
          tm_struct->tm_year+1900, tm_struct->tm_mon+1, tm_struct->tm_mday,
          tm_struct->tm_hour, tm_struct->tm_min, tm_struct->tm_sec);
  out << "Dump event time:  "<< timebuf << "\n";
      snprintf(timebuf, sizeof(timebuf), "%4d/%02d/%02d %02d:%02d:%02d",
          loadtime_tm_struct.tm_year+1900, loadtime_tm_struct.tm_mon+1, loadtime_tm_struct.tm_mday,
          loadtime_tm_struct.tm_hour, loadtime_tm_struct.tm_min, loadtime_tm_struct.tm_sec);
  out << "Module load time: " << timebuf << "\n";
#endif
  // Print native process ID
  out << "Process ID: " << pid << std::endl;


  // Print out the command line.
  PrintCommandLine(out);
  out << std::flush;

  // Print Node.js and OS version information
  PrintVersionInformation(out);
  out << std::flush;

// Print summary JavaScript stack backtrace
  PrintJavaScriptStack(out, isolate, event, location);
  out << std::flush;

  // Print native stack backtrace
  PrintNativeStack(out);
  out << std::flush;

  // Print the stack trace and message from the Error object.
  // (If one was provided.)
  PrintJavaScriptErrorStack(out, isolate, error);
  out << std::flush;

  // Print V8 Heap and Garbage Collector information
  PrintGCStatistics(out, isolate);
  out << std::flush;

  // Print OS and current thread resource usage
#ifndef _WIN32
  PrintResourceUsage(out);
  out << std::flush;
#endif

  // Print libuv handle summary
  out << "\n================================================================================";
  out << "\n==== Node.js libuv Handle Summary ==============================================\n";
  out << "\n(Flags: R=Ref, A=Active)\n";
  out << std::left << std::setw(7) << "Flags" << std::setw(10) << "Type"
      << std::setw(4 + 2 * sizeof(void*)) << "Address" << "Details"
      << std::endl;
  uv_walk(uv_default_loop(), walkHandle, (void*)&out);

  // Print operating system information
  PrintSystemInformation(out, isolate);

  out << "\n================================================================================\n";
  out << std::flush;

  // Restore output stream formatting.
  out.copyfmt(oldState);

  report_active = false;
}

/*******************************************************************************
 * Function to print process command line.
 *
 ******************************************************************************/
static void PrintCommandLine(std::ostream& out) {
  if (commandline_string != "") {
    out << "Command line: " << commandline_string << "\n";
  }
}

/*******************************************************************************
 * Function to print Node.js version, OS version and machine information
 *
 ******************************************************************************/
static void PrintVersionInformation(std::ostream& out) {

  // Print Node.js and deps component versions
  out << "\n" << version_string;

  // Print node-report module version
  // e.g. node-report version: 1.0.6 (built against Node.js v6.9.1)
  out << std::endl << "node-report version: " << NODEREPORT_VERSION
      << " (built against Node.js v" << NODE_VERSION_STRING;
#if defined(__GLIBC__)
  out << ", glibc " << __GLIBC__ << "." << __GLIBC_MINOR__;
#endif
  // Print Process word size
  out << ", " << sizeof(void *) * 8 << " bit" << ")" << std::endl;

  // Print operating system and machine information (Windows)
#ifdef _WIN32
  {
    const DWORD level = 101;
    LPSERVER_INFO_101 os_info = NULL;
    NET_API_STATUS nStatus = NetServerGetInfo(NULL, level, (LPBYTE *)&os_info);
    if (nStatus == NERR_Success) {
      LPSTR os_name = "Windows";
      const DWORD major = os_info->sv101_version_major & MAJOR_VERSION_MASK;
      const DWORD type = os_info->sv101_type;
      const bool isServer = (type & SV_TYPE_DOMAIN_CTRL) ||
                            (type & SV_TYPE_DOMAIN_BAKCTRL) ||
                            (type & SV_TYPE_SERVER_NT);
      switch (major) {
        case 5:
          switch (os_info->sv101_version_minor) {
            case 0:
              os_name = "Windows 2000";
              break;
            default:
              os_name = (isServer ? "Windows Server 2003" : "Windows XP");
          }
          break;
        case 6:
          switch (os_info->sv101_version_minor) {
            case 0:
              os_name = (isServer ? "Windows Server 2008" : "Windows Vista");
              break;
            case 1:
              os_name = (isServer ? "Windows Server 2008 R2" : "Windows 7");
              break;
            case 2:
              os_name = (isServer ? "Windows Server 2012" : "Windows 8");
              break;
            case 3:
              os_name = (isServer ? "Windows Server 2012 R2" : "Windows 8.1");
              break;
            default:
              os_name = (isServer ? "Windows Server" : "Windows Client");
          }
          break;
        case 10:
          os_name = (isServer ? "Windows Server 2016" : "Windows 10");
          break;
        default:
          os_name = (isServer ? "Windows Server" : "Windows Client");
      }
      out <<  "\nOS version: " << os_name << "\n";

      // Convert and print the machine name and comment fields (these are LPWSTR types)
      size_t count;
      char name_buf[256];
      wcstombs_s(&count, name_buf, sizeof(name_buf), os_info->sv101_name, _TRUNCATE);
      if (os_info->sv101_comment != NULL) {
        char comment_buf[256];
        wcstombs_s(&count, comment_buf, sizeof(comment_buf), os_info->sv101_comment, _TRUNCATE);
        out << "\nMachine: " << name_buf << " " << comment_buf << "\n";
      } else {
        out << "\nMachine: " << name_buf << "\n";
      }

      if (os_info != NULL) {
        NetApiBufferFree(os_info);
      }
    } else {
      // NetServerGetInfo() call failed, fallback to use GetComputerName() instead
      TCHAR machine_name[256];
      DWORD machine_name_size = 256;
      out << "\nOS version: Windows\n";
      if (GetComputerName(machine_name, &machine_name_size)) {
        out << "\nMachine: " << machine_name << "\n";
      }
    }
  }
#else
  // Print operating system and machine information (Unix/OSX)
  struct utsname os_info;
  if (uname(&os_info) >= 0) {
#if defined(_AIX)
    out << "\nOS version: " << os_info.sysname << " " << os_info.version << "."
        << os_info.release << "\n";
#else
    out << "\nOS version: " << os_info.sysname << " " << os_info.release << " "
        << os_info.version << "\n";
#endif
    const char *(*libc_version)();
    *(void**)(&libc_version) = dlsym(RTLD_DEFAULT, "gnu_get_libc_version");
    if (libc_version != NULL) {
      out << "(glibc: " << (*libc_version)() << ")" << std::endl;
    }
    out <<  "\nMachine: " << os_info.nodename << " " << os_info.machine << "\n";
  }
#endif
}

/*******************************************************************************
 * Function to print the JavaScript stack, if available
 *
 ******************************************************************************/
static void PrintJavaScriptStack(std::ostream& out, Isolate* isolate, DumpEvent event, const char* location) {
  out << "\n================================================================================";
  out << "\n==== JavaScript Stack Trace ====================================================\n\n";

#ifdef _WIN32
  switch (event) {
  case kFatalError:
    // Stack trace on fatal error not supported on Windows
    out << "No stack trace available\n";
    break;
  default:
    // All other events, print the stack using StackTrace::StackTrace() and GetStackSample() APIs
    PrintStackFromStackTrace(out, isolate, event);
    break;
  }  // end switch(event)
#else  // Unix, OSX
  switch (event) {
  case kException:
  case kJavaScript: {
    // Print the stack using Message::PrintCurrentStackTrace() API
    std::FILE *stack_fp = std::tmpfile();
    if (stack_fp != nullptr) {
      char stack_buf[64];
      Message::PrintCurrentStackTrace(isolate, stack_fp);
      std::fflush(stack_fp);
      std::rewind(stack_fp);
      while (std::fgets(stack_buf, sizeof(stack_buf), stack_fp) != nullptr) {
        out << stack_buf;
      }
      // Calling close on a file from tmpfile *should* delete it.
      std::fclose(stack_fp);
    } else {
      out << "No stack trace available, unable to create temporary file\n";
    }
    break;
  }
  case kFatalError:
    out << "No stack trace available\n";
    break;
  case kSignal_JS:
  case kSignal_UV:
    // Print the stack using StackTrace::StackTrace() and GetStackSample() APIs
    PrintStackFromStackTrace(out, isolate, event);
    break;
  }  // end switch(event)
#endif
}

static void PrintJavaScriptErrorStack(std::ostream& out, Isolate* isolate, MaybeLocal<Value> error) {
  if (error.IsEmpty() || !error.ToLocalChecked()->IsNativeError()) {
    return;
  }

  out << "\n================================================================================";
  out << "\n==== JavaScript Exception Details ==============================================\n\n";
#if NODE_MAJOR_VERSION > 5
  Local<Message> message = v8::Exception::CreateMessage(isolate, error.ToLocalChecked());
#else
  Local<Message> message = v8::Exception::CreateMessage(error.ToLocalChecked());
#endif
  Nan::Utf8String message_str(message->Get());

  out << *message_str << "\n\n";

  Local<StackTrace> stack = v8::Exception::GetStackTrace(error.ToLocalChecked());
  if (stack.IsEmpty()) {
     out << "No stack trace available from Exception::GetStackTrace()\n";
     return;
  }
  // Print the stack trace, samples are not available as the exception isn't from the current stack.
  for (int i = 0; i < stack->GetFrameCount(); i++) {
    PrintStackFrame(out, isolate, stack->GetFrame(i), i, nullptr);
  }
}

/*******************************************************************************
 * Function to print stack using GetStackSample() and StackTrace::StackTrace()
 *
 ******************************************************************************/
static void PrintStackFromStackTrace(std::ostream& out, Isolate* isolate, DumpEvent event) {
  v8::RegisterState state;
  v8::SampleInfo info;
  void* samples[255];

  // Initialise the register state
  state.pc = nullptr;
  state.fp = &state;
  state.sp = &state;

  isolate->GetStackSample(state, samples, arraysize(samples), &info);
  if (static_cast<size_t>(info.vm_state) < arraysize(v8_states)) {
    out << "JavaScript VM state: " << v8_states[info.vm_state] << "\n\n";
  } else {
    out << "JavaScript VM state: <unknown>\n\n";
  }
  if (event == kSignal_UV) {
    out << "Signal received when event loop idle, no stack trace available\n";
    return;
  }
  Local<StackTrace> stack = StackTrace::CurrentStackTrace(isolate, 255, StackTrace::kDetailed);
  if (stack.IsEmpty()) {
    out << "\nNo stack trace available from StackTrace::CurrentStackTrace()\n";
    return;
  }
  // Print the stack trace, adding in the pc values from GetStackSample() if available
  for (int i = 0; i < stack->GetFrameCount(); i++) {
    if (static_cast<size_t>(i) < info.frames_count) {
      PrintStackFrame(out, isolate, stack->GetFrame(i), i, samples[i]);
    } else {
      PrintStackFrame(out, isolate, stack->GetFrame(i), i, nullptr);
    }
  }
}

/*******************************************************************************
 * Function to print a JavaScript stack frame from a V8 StackFrame object
 *
 ******************************************************************************/
static void PrintStackFrame(std::ostream& out, Isolate* isolate, Local<StackFrame> frame, int i, void* pc) {
  Nan::Utf8String fn_name_s(frame->GetFunctionName());
  Nan::Utf8String script_name(frame->GetScriptName());
  const int line_number = frame->GetLineNumber();
  const int column = frame->GetColumn();
  char buf[64];

  // First print the frame index and the instruction address
  if (pc != nullptr) {
#ifdef _WIN32
    snprintf( buf, sizeof(buf), "%2d: [pc=0x%p] ", i, pc);
#else
    snprintf( buf, sizeof(buf), "%2d: [pc=%p] ", i, pc);
#endif
    out << buf;
  }

  // Now print the JavaScript function name and source information
  if (frame->IsEval()) {
    if (frame->GetScriptId() == Message::kNoScriptIdInfo) {
      out << "at [eval]:" << line_number << ":" << column << "\n";
    } else {
      out << "at [eval] (" << *script_name << ":" << line_number << ":"
          << column << ")\n";
    }
    return;
  }

  if (fn_name_s.length() == 0) {
    out << *script_name << ":" << line_number << ":" << column << "\n";
  } else {
    if (frame->IsConstructor()) {
      out << *fn_name_s << " [constructor] (" << *script_name << ":"
          << line_number << ":" << column << ")\n";
    } else {
      out << *fn_name_s << " (" << *script_name << ":" << line_number << ":"
          << column << ")\n";
    }
  }
}


#ifdef _WIN32
/*******************************************************************************
 * Function to print a native stack backtrace
 *
 ******************************************************************************/
void PrintNativeStack(std::ostream& out) {
  void* frames[64];
  out << "\n================================================================================";
  out << "\n==== Native Stack Trace ========================================================\n\n";

  HANDLE hProcess = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(hProcess, nullptr, TRUE);
  char buf[64];

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
      line.SizeOfStruct = sizeof(line);
      snprintf(buf, sizeof(buf), "%2d: [pc=0x%p]", i, reinterpret_cast<void*>(pSymbol->Address));
      out << buf << " " << pSymbol->Name << " [+";
      if (SymGetLineFromAddr64(hProcess, dwAddress, &dwOffset, &line)) {
        out <<  dwOffset << "] in " << line.FileName << ": line: " << line.LineNumber << "\n";
      } else {
        out << dwOffset64 << "]\n";
      }
    } else { // SymFromAddr() failed, just print the address
      snprintf(buf, sizeof(buf), "%2d: [pc=0x%p]\n", i, reinterpret_cast<void*>(dwAddress));
      out << buf;
    }
  }
}
#elif _AIX
/*******************************************************************************
 * Function to print a native stack backtrace - AIX
 *
 ******************************************************************************/
void PrintNativeStack(std::ostream& out) {
  out << "\n================================================================================";
  out << "\n==== Native Stack Trace ========================================================\n\n";
  out << "Native stack trace not supported on AIX\n";
}
#else
/*******************************************************************************
 * Function to print a native stack backtrace - Linux/OSX
 *
 ******************************************************************************/
void PrintNativeStack(std::ostream& out) {
  void* frames[256];
  char buf[64];
  out << "\n================================================================================";
  out << "\n==== Native Stack Trace ========================================================\n\n";

  // Get the native backtrace (array of instruction addresses)
  const int size = backtrace(frames, arraysize(frames));
  if (size <= 0) {
    out << "Native backtrace failed, error " << size << "\n";
    return;
  } else if (size <=2) {
    out << "No frames to print\n";
    return;
  }

  // Print the native frames, omitting the top 3 frames as they are in node-report code
  // backtrace_symbols_fd(frames, size, fileno(fp));
  for (int i = 2; i < size; i++) {
    // print frame index and instruction address
    snprintf(buf, sizeof(buf), "%2d: [pc=%p] ", i-2, frames[i]);
    out << buf;
    // If we can translate the address using dladdr() print additional symbolic information
    Dl_info info;
    if (dladdr(frames[i], &info)) {
      if (info.dli_sname != nullptr) {
        if (char* demangled = abi::__cxa_demangle(info.dli_sname, 0, 0, 0)) {
          out << demangled; // print demangled symbol name
          free(demangled);
        } else {
          out << info.dli_sname; // just print the symbol name
        }
      }
      if (info.dli_fname != nullptr) {
        out << " [" << info.dli_fname << "]"; // print shared object name
      }
    }
    out << std::endl;
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
static void PrintGCStatistics(std::ostream& out, Isolate* isolate) {
  HeapStatistics v8_heap_stats;
  isolate->GetHeapStatistics(&v8_heap_stats);

  out << "\n================================================================================";
  out << "\n==== JavaScript Heap and Garbage Collector =====================================\n";
  HeapSpaceStatistics v8_heap_space_stats;
  // Loop through heap spaces
  for (size_t i = 0; i < isolate->NumberOfHeapSpaces(); i++) {
    isolate->GetHeapSpaceStatistics(&v8_heap_space_stats, i);
    out << "\nHeap space name: " << v8_heap_space_stats.space_name();
    out << "\n    Memory size: ";
    WriteInteger(out, v8_heap_space_stats.space_size());
    out << " bytes, committed memory: ";
    WriteInteger(out, v8_heap_space_stats.physical_space_size());
    out << " bytes\n    Capacity: ";
    WriteInteger(out, v8_heap_space_stats.space_used_size() +
                           v8_heap_space_stats.space_available_size());
    out << " bytes, used: ";
    WriteInteger(out, v8_heap_space_stats.space_used_size());
    out << " bytes, available: ";
    WriteInteger(out, v8_heap_space_stats.space_available_size());
    out << " bytes";
  }

  out << "\n\nTotal heap memory size: ";
  WriteInteger(out, v8_heap_stats.total_heap_size());
  out << " bytes\nTotal heap committed memory: ";
  WriteInteger(out, v8_heap_stats.total_physical_size());
  out << " bytes\nTotal used heap memory: ";
  WriteInteger(out, v8_heap_stats.used_heap_size());
  out << " bytes\nTotal available heap memory: ";
  WriteInteger(out, v8_heap_stats.total_available_size());
  out << " bytes\n\nHeap memory limit: ";
  WriteInteger(out, v8_heap_stats.heap_size_limit());
  out << "\n";
}

#ifndef _WIN32
/*******************************************************************************
 * Function to print resource usage (Linux/OSX only).
 *
 ******************************************************************************/
static void PrintResourceUsage(std::ostream& out) {
  char buf[64];
  double cpu_abs;
  double cpu_percentage;
  time_t current_time; // current time absolute
  time(&current_time);
  auto uptime = difftime(current_time, load_time);
  if (uptime == 0)
    uptime = 1; // avoid division by zero.
  out << "\n================================================================================";
  out << "\n==== Resource Usage ============================================================\n";

  // Process and current thread usage statistics
  struct rusage stats;
  out << "\nProcess total resource usage:";
  if (getrusage(RUSAGE_SELF, &stats) == 0) {
#if defined(__APPLE__) || defined(_AIX)
    snprintf( buf, sizeof(buf), "%ld.%06d", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    out << "\n  User mode CPU: " << buf << " secs";
    snprintf( buf, sizeof(buf), "%ld.%06d", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
    out << "\n  Kernel mode CPU: " << buf << " secs";
#else
    snprintf( buf, sizeof(buf), "%ld.%06ld", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    out << "\n  User mode CPU: " << buf << " secs";
    snprintf( buf, sizeof(buf), "%ld.%06ld", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
    out << "\n  Kernel mode CPU: " << buf << " secs";
#endif
    cpu_abs = stats.ru_utime.tv_sec + 0.000001 * stats.ru_utime.tv_usec + stats.ru_stime.tv_sec + 0.000001 *  stats.ru_stime.tv_usec;
    cpu_percentage = (cpu_abs / uptime) * 100.0;
    out << "\n  Average CPU Consumption : "<< cpu_percentage << "%";
    out << "\n  Maximum resident set size: ";
    WriteInteger(out, stats.ru_maxrss * 1024);
    out << " bytes\n  Page faults: " << stats.ru_majflt << " (I/O required) "
        << stats.ru_minflt << " (no I/O required)";
    out << "\n  Filesystem activity: " << stats.ru_inblock << " reads "
        <<  stats.ru_oublock << " writes";
  }
#ifdef RUSAGE_THREAD
  out << "\n\nEvent loop thread resource usage:";
  if (getrusage(RUSAGE_THREAD, &stats) == 0) {
#if defined(__APPLE__) || defined(_AIX)
    snprintf( buf, sizeof(buf), "%ld.%06d", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    out << "\n  User mode CPU: " << buf << " secs";
    snprintf( buf, sizeof(buf), "%ld.%06d", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
    out << "\n  Kernel mode CPU: " << buf << " secs";
#else
    snprintf( buf, sizeof(buf), "%ld.%06ld", stats.ru_utime.tv_sec, stats.ru_utime.tv_usec);
    out << "\n  User mode CPU: " << buf << " secs";
    snprintf( buf, sizeof(buf), "%ld.%06ld", stats.ru_stime.tv_sec, stats.ru_stime.tv_usec);
    out << "\n  Kernel mode CPU: " << buf << " secs";
#endif
    cpu_abs = stats.ru_utime.tv_sec + 0.000001 * stats.ru_utime.tv_usec + stats.ru_stime.tv_sec + 0.000001 *  stats.ru_stime.tv_usec;
    cpu_percentage = (cpu_abs / uptime) * 100.0;
    out << "\n  Average CPU Consumption : " << cpu_percentage << "%";
    out << "\n  Filesystem activity: " << stats.ru_inblock << " reads "
        << stats.ru_oublock << " writes";
  }
#endif
  out << std::endl;
}
#endif

/*******************************************************************************
 * Function to print operating system information.
 *
 ******************************************************************************/
static void PrintSystemInformation(std::ostream& out, Isolate* isolate) {
  out << "\n================================================================================";
  out << "\n==== System Information ========================================================\n";

#ifdef _WIN32
  out << "\nEnvironment variables\n";
  LPTSTR lpszVariable;
  LPTCH lpvEnv;

  // Get pointer to the environment block
  lpvEnv = GetEnvironmentStrings();
  if (lpvEnv != nullptr) {
    // Variable strings are separated by null bytes, and the block is terminated by a null byte.
    lpszVariable = reinterpret_cast<LPTSTR>(lpvEnv);
    while (*lpszVariable) {
      out << "  " << lpszVariable << "\n", lpszVariable;
      lpszVariable += lstrlen(lpszVariable) + 1;
    }
    FreeEnvironmentStrings(lpvEnv);
  }
#else
  out << "\nEnvironment variables\n";
  int index = 1;
  char* env_var = *environ;

  while (env_var != nullptr) {
    out << "  " << env_var << "\n";
    env_var = *(environ + index++);
  }

const static struct {
  const char* description;
  int id;
} rlimit_strings[] = {
  {"core file size (blocks)       ", RLIMIT_CORE},
  {"data seg size (kbytes)        ", RLIMIT_DATA},
  {"file size (blocks)            ", RLIMIT_FSIZE},
#if !(defined(_AIX) || defined(__sun))
  {"max locked memory (bytes)     ", RLIMIT_MEMLOCK},
#endif
#ifndef __sun
  {"max memory size (kbytes)      ", RLIMIT_RSS},
#endif
  {"open files                    ", RLIMIT_NOFILE},
  {"stack size (bytes)            ", RLIMIT_STACK},
  {"cpu time (seconds)            ", RLIMIT_CPU},
#ifndef __sun
  {"max user processes            ", RLIMIT_NPROC},
#endif
  {"virtual memory (kbytes)       ", RLIMIT_AS}
};

  out << "\nResource limits                        soft limit      hard limit\n";
  struct rlimit limit;
  char buf[64];

  for (size_t i = 0; i < arraysize(rlimit_strings); i++) {
    if (getrlimit(rlimit_strings[i].id, &limit) == 0) {
      out << "  " << rlimit_strings[i].description << " ";
      if (limit.rlim_cur == RLIM_INFINITY) {
        out << "       unlimited";
      } else {
#if defined(_AIX) || defined(__sun)
        snprintf(buf, sizeof(buf), "%16ld", limit.rlim_cur);
        out << buf;
#else
        snprintf(buf, sizeof(buf), "%16" PRIu64, limit.rlim_cur);
        out << buf;
#endif
      }
      if (limit.rlim_max == RLIM_INFINITY) {
        out << "       unlimited\n";
      } else {
#ifdef _AIX
        snprintf(buf, sizeof(buf), "%16ld\n", limit.rlim_max);
        out << buf;
#else
        snprintf(buf, sizeof(buf), "%16" PRIu64 "\n", limit.rlim_max);
        out << buf;
#endif
      }
    }
  }
#endif

  out << "\nLoaded libraries\n";
  PrintLoadedLibraries(out, isolate);
}

/*******************************************************************************
 * Functions to print a list of loaded native libraries.
 *
 ******************************************************************************/
#ifdef __linux__
static int LibraryPrintCallback(struct dl_phdr_info *info, size_t size, void *data) {
  std::ostream* out = reinterpret_cast<std::ostream*>(data);
  if (info->dlpi_name != nullptr && *info->dlpi_name != '\0') {
    *out << "  " << info->dlpi_name << "\n";
  }
  return 0;
}
#endif

static void PrintLoadedLibraries(std::ostream& out, Isolate* isolate) {
#ifdef __linux__
  dl_iterate_phdr(LibraryPrintCallback, &out);
#elif __APPLE__
  int i = 0;
  const char *name = _dyld_get_image_name(i);
  while (name != nullptr) {
    out << "  " << name << "\n";
    i++;
    name = _dyld_get_image_name(i);
  }
#elif _AIX
  // We can't tell in advance how large the buffer needs to be.
  // Retry until we reach too large a size (1Mb).
  const unsigned int buffer_inc = 4096;
  unsigned int buffer_size = buffer_inc;
  char* buffer = (char*) malloc(buffer_size);
  int rc = -1;
  while (buffer != nullptr && rc != 0 && buffer_size < 1024 * 1024) {
    rc = loadquery(L_GETINFO, buffer, buffer_size);
    if (rc == 0) {
      break;
    }
    free(buffer);
    buffer_size += buffer_inc;
    buffer = (char*) malloc(buffer_size);
  }
  if (buffer == nullptr) {
    return; // Don't try to free the buffer.
  }
  if (rc == 0) {
    char* buf = buffer;
    ld_info* cur_info = nullptr;
    do {
      cur_info = (ld_info*) buf;
      char* member_name = cur_info->ldinfo_filename
        + strlen(cur_info->ldinfo_filename) + 1;
      if (*member_name != '\0') {
        out << "  " << cur_info->ldinfo_filename << "(" << member_name << ")\n";
      } else {
        out << "  " << cur_info->ldinfo_filename << "\n";
      }
      buf += cur_info->ldinfo_next;
    } while (cur_info->ldinfo_next != 0);
  }
  free(buffer);
#elif __sun
  Link_map *p;

  if (dlinfo(RTLD_SELF, RTLD_DI_LINKMAP, &p) != -1) {
    for (Link_map *l = p; l != NULL; l = l->l_next) {
      out << "  " << l->l_name << "\n";
    }
  }

#elif _WIN32
  // Windows implementation - get a handle to the process.
  HANDLE process_handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                      FALSE, GetCurrentProcessId());
  if (process_handle == NULL) {
    out << "No library information available\n";
    return;
  }
  // Get a list of all the modules in this process
  DWORD size_1 = 0, size_2 = 0;
  // First call to get the size of module array needed
  if (EnumProcessModules(process_handle, NULL, 0, &size_1)) {
    HMODULE* modules = (HMODULE*) malloc(size_1);
    if (modules == NULL) {
      return;  // bail out if malloc failed
    }
    // Second call to populate the module array
    if (EnumProcessModules(process_handle, modules, size_1, &size_2)) {
      for (int i = 0;
           i < (size_1 / sizeof(HMODULE)) && i < (size_2 / sizeof(HMODULE));
           i++) {
        TCHAR module_name[MAX_PATH];
        // Obtain and print the full pathname for each module
        if (GetModuleFileNameEx(process_handle, modules[i], module_name,
                                sizeof(module_name) / sizeof(TCHAR))) {
          out << "  " << module_name << "\n";
        }
      }
    }
    free(modules);
  } else {
    out << "No library information available\n";
  }
  // Release the handle to the process.
  CloseHandle(process_handle);
#endif
}

/*******************************************************************************
 * Utility function to print out integer values with commas for readability.
 *
 ******************************************************************************/
static void WriteInteger(std::ostream& out, size_t value) {
  int thousandsStack[8];  // Sufficient for max 64-bit number
  int stackTop = 0;
  int i;
  char buf[64];
  size_t workingValue = value;

  do {
    thousandsStack[stackTop++] = workingValue % 1000;
    workingValue /= 1000;
  } while (workingValue != 0);

  for (i = stackTop-1; i >= 0; i--) {
    if (i == (stackTop-1)) {
      out << thousandsStack[i];
    } else {
      snprintf(buf, sizeof(buf), "%03u", thousandsStack[i]);
      out << buf;
    }
    if (i > 0) {
      out << ",";
    }
  }
}

}  // namespace nodereport
