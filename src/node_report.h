#ifndef SRC_NODE_REPORT_H_
#define SRC_NODE_REPORT_H_

#include "nan.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#endif

namespace nodereport {

using v8::Isolate;
using v8::Local;
using v8::Message;
using v8::Function;
using v8::Object;
using v8::Number;
using v8::String;
using v8::Value;
using v8::StackTrace;
using v8::StackFrame;
using v8::MaybeLocal;

// Bit-flags for node-report trigger options
#define NR_EXCEPTION  0x01
#define NR_FATALERROR 0x02
#define NR_SIGNAL     0x04
#define NR_APICALL    0x08

// Maximum file and path name lengths
#define NR_MAXNAME 64
#define NR_MAXPATH 1024

enum DumpEvent {kException, kFatalError, kSignal_JS, kSignal_UV, kJavaScript};

void TriggerNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, char* name, v8::MaybeLocal<v8::Value> error);
void GetNodeReport(Isolate* isolate, DumpEvent event, const char* message, const char* location, v8::MaybeLocal<v8::Value> error, std::ostream& out);

unsigned int ProcessNodeReportEvents(const char* args);
unsigned int ProcessNodeReportSignal(const char* args);
void ProcessNodeReportFileName(const char* args);
void ProcessNodeReportDirectory(const char* args);
unsigned int ProcessNodeReportVerboseSwitch(const char* args);

void SetLoadTime();
void SetVersionString(Isolate* isolate);
void SetCommandLine();

// Local implementation of secure_getenv()
inline const char* secure_getenv(const char* key) {
#ifndef _WIN32
  if (getuid() != geteuid() || getgid() != getegid())
    return nullptr;
#endif
  return getenv(key);
}

// Emulate arraysize() on Windows pre Visual Studio 2015
#if defined(_MSC_VER) && _MSC_VER < 1900
#define arraysize(a) (sizeof(a) / sizeof(*a))
#else
template <typename T, size_t N>
constexpr size_t arraysize(const T(&)[N]) { return N; }
#endif  // defined( _MSC_VER ) && (_MSC_VER < 1900)

// Emulate snprintf() on Windows pre Visual Studio 2015
#if defined( _MSC_VER ) && (_MSC_VER < 1900)
#include <stdarg.h>
inline static int snprintf(char* buffer, size_t n, const char* format, ...) {
  va_list argp;
  va_start(argp, format);
  int ret = _vscprintf(format, argp);
  vsnprintf_s(buffer, n, _TRUNCATE, format, argp);
  va_end(argp);
  return ret;
}

#define __func__ __FUNCTION__
#endif  // defined( _MSC_VER ) && (_MSC_VER < 1900)

}  // namespace nodereport

#endif  // SRC_NODE_REPORT_H_
