#ifndef SRC_NODE_REPORT_H_
#define SRC_NODE_REPORT_H_

#include "nan.h"
#if !defined(_WIN32) && !defined(__APPLE__)
#include <features.h>
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

// Bit-flags for NodeReport trigger options
#define NR_EXCEPTION  0x01
#define NR_FATALERROR 0x02
#define NR_SIGNAL     0x04
#define NR_JSAPICALL  0x08

// Maximum file and path name lengths
#define NR_MAXNAME 64
#define NR_MAXPATH 1024

enum DumpEvent {kException, kFatalError, kSignal_JS, kSignal_UV, kJavaScript};

void TriggerNodeReport(Isolate* isolate, DumpEvent event, const char *message, const char *location, char* name);

unsigned int ProcessNodeReportEvents(const char *args);
unsigned int ProcessNodeReportCoreSwitch(const char *args);
unsigned int ProcessNodeReportSignal(const char *args);
void ProcessNodeReportFileName(const char *args);
void ProcessNodeReportDirectory(const char *args);
unsigned int ProcessNodeReportVerboseSwitch(const char *args);

void SetLoadTime();

// secure_getenv() only available in glibc, revert to getenv() otherwise
#if defined(__GLIBC__)
#if !__GLIBC_PREREQ(2, 17)
#define secure_getenv getenv
#endif  // !__GLIBC_PREREQ(2, 17)
#else
#define secure_getenv getenv
#endif  // defined(__GLIBC__)

}  // namespace nodereport

#endif  // SRC_NODE_REPORT_H_
