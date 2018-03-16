//
// c4Base.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Internal.hh"
#include "c4Database.h"
#include "c4Document.h"
#include "c4Private.h"

#include "Logging.hh"
#include "StringUtil.hh"

#include "WebSocketInterface.hh"
#include "WebSocketImpl.hh"         // just for WSLogDomain
#include "SQLiteCpp/Exception.h"
#include "repo_version.h"    // Generated by get_repo_version.sh at build time
#include <ctype.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <set>

#ifdef _MSC_VER
#include <winerror.h>
#endif

#if defined(__clang__) // For __cxa_demangle
#include <cxxabi.h>
#endif


using namespace litecore;

extern "C" {
    CBL_CORE_API std::atomic_int gC4InstanceCount;
    CBL_CORE_API std::atomic_int gC4ExpectExceptions;
    bool C4ExpectingExceptions();
    bool C4ExpectingExceptions() { return gC4ExpectExceptions > 0; } // LCOV_EXCL_LINE
}


// LCOV_EXCL_START
static string getBuildInfo() {
#if LiteCoreOfficial
    return format("build number %s from commit %.8s", LiteCoreBuildNum, GitCommit);
#else
    if (strcmp(GitBranch, "HEAD") == (0))
        return format("built from commit %.8s%s on %s %s",
                      GitCommit, GitDirty, __DATE__, __TIME__);
    else
        return format("built from %s branch, commit %.8s%s on %s %s",
                      GitBranch, GitCommit, GitDirty, __DATE__, __TIME__);
#endif
}


C4StringResult c4_getBuildInfo() C4API {
    return sliceResult(getBuildInfo());
}


C4StringResult c4_getVersion() C4API {
    string vers;
#if LiteCoreOfficial
    vers = LiteCoreBuildNum;
#else
    if (strcmp(GitBranch, "master") == (0) || strcmp(GitBranch, "HEAD") == (0))
        vers = format("%.8s%.1s", GitCommit, GitDirty);
    else
        vers = format("%s:%.8s%.1s", GitBranch, GitCommit, GitDirty);
#endif
    return sliceResult(vers);
}
// LCOV_EXCL_STOP

#pragma mark - ERRORS:


namespace c4Internal {

    static_assert((int)kC4MaxErrorDomainPlus1 == (int)error::NumDomainsPlus1,
                  "C4 error domains are not in sync with C++ ones");
    static_assert((int)kC4NumErrorCodesPlus1 == (int)error::NumLiteCoreErrorsPlus1,
                  "C4 error codes are not in sync with C++ ones");

    // A buffer that stores recently generated error messages, referenced by C4Error.internal_info
    static uint32_t sFirstErrorMessageInternalInfo = 1000;  // internal_info of 1st item in deque
    static deque<string> sErrorMessages;                    // last 10 error message strings
    static mutex sErrorMessagesMutex;


    void recordError(C4ErrorDomain domain, int code, string message, C4Error* outError) noexcept {
        if (outError) {
            outError->domain = domain;
            outError->code = code;
            outError->internal_info = 0;
            if (!message.empty()) {
                try {
                    lock_guard<mutex> lock(sErrorMessagesMutex);
                    sErrorMessages.emplace_back(message);
                    if (sErrorMessages.size() > kMaxErrorMessagesToSave) {
                        sErrorMessages.pop_front();
                        ++sFirstErrorMessageInternalInfo;
                    }
                    outError->internal_info = (uint32_t)(sFirstErrorMessageInternalInfo +
                                                         sErrorMessages.size() - 1);
                } catch (...) { }
            }
        }
    }

    void recordError(C4ErrorDomain domain, int code, C4Error* outError) noexcept {
        recordError(domain, code, string(), outError);
    }

    static string lookupErrorMessage(C4Error &error) {
        lock_guard<mutex> lock(sErrorMessagesMutex);
        int32_t index = error.internal_info - sFirstErrorMessageInternalInfo;
        if (index >= 0 && index < sErrorMessages.size()) {
            return sErrorMessages[index];
        } else {
            return string();
        }
    }
}


C4Error c4error_make(C4ErrorDomain domain, int code, C4String message) C4API {
    C4Error error;
    recordError(domain, code, (string)message, &error);
    return error;
}


void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) C4API {
    recordError(domain, code, (string)message, outError);
}


C4SliceResult c4error_getMessage(C4Error err) noexcept {
    if (err.code == 0) {
        return sliceResult(nullptr);
    } else if (err.domain < 1 || err.domain >= (C4ErrorDomain)error::NumDomainsPlus1) {
        return sliceResult("unknown error domain");
    } else {
        // Custom message referenced in info field?
        string message = lookupErrorMessage(err);
        if (!message.empty())
            return sliceResult(message);
        // No; get the regular error message for this domain/code:
        error e((error::Domain)err.domain, err.code);
        return sliceResult(e.what());
    }
}

char* c4error_getMessageC(C4Error error, char buffer[], size_t bufferSize) noexcept {
    C4SliceResult msg = c4error_getMessage(error);
    auto len = min(msg.size, bufferSize-1);
    if (msg.buf)
        memcpy(buffer, msg.buf, len);
    buffer[len] = '\0';
    c4slice_free(msg);
    return buffer;
}


#pragma mark - ERROR UTILITIES:


using CodeList = const int[];
using ErrorSet = const int* [kC4MaxErrorDomainPlus1];


static bool errorIsInSet(C4Error err, ErrorSet set) {
    if (err.code != 0 && (unsigned)err.domain < kC4MaxErrorDomainPlus1) {
        const int *pCode = set[err.domain];
        if (pCode) {
            for (; *pCode != 0; ++pCode)
                if (*pCode == err.code)
                    return true;
        }
    }
    return false;
}


bool c4error_mayBeTransient(C4Error err) C4API {
    static CodeList kTransientPOSIX = {
        ENETRESET, ECONNABORTED, ECONNRESET, ETIMEDOUT, ECONNREFUSED, 0};

    static CodeList kTransientNetwork = {
        kC4NetErrDNSFailure,
        0};
    static CodeList kTransientWebSocket = {
        408, /* Request Timeout */
        429, /* Too Many Requests (RFC 6585) */
        500, /* Internal Server Error */
        502, /* Bad Gateway */
        503, /* Service Unavailable */
        504, /* Gateway Timeout */
        websocket::kCodeGoingAway,
        0};
    static ErrorSet kTransient = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kTransientPOSIX,
        nullptr,
        nullptr,
        kTransientNetwork,
        kTransientWebSocket};
    return errorIsInSet(err, kTransient);
}

bool c4error_mayBeNetworkDependent(C4Error err) C4API {
    static CodeList kUnreachablePOSIX = {
        ENETDOWN, ENETUNREACH, ENOTCONN, ETIMEDOUT, 
#ifndef _MSC_VER
        EHOSTDOWN, // Doesn't exist on Windows
#endif
        EHOSTUNREACH,EADDRNOTAVAIL, 0};

    static CodeList kUnreachableNetwork = {
        kC4NetErrDNSFailure,
        kC4NetErrUnknownHost,   // Result may change if user logs into VPN or moves to intranet
        0};
    static ErrorSet kUnreachable = { // indexed by C4ErrorDomain
        nullptr,
        nullptr,
        kUnreachablePOSIX,
        nullptr,
        nullptr,
        kUnreachableNetwork,
        nullptr};
    return errorIsInSet(err, kUnreachable);
}


#pragma mark - SLICES:


bool c4SliceEqual(C4Slice a, C4Slice b) noexcept {
    return a == b;
}


void c4slice_free(C4SliceResult slice) noexcept {
    alloc_slice::release({slice.buf, slice.size});
}


namespace c4Internal {

    C4SliceResult sliceResult(alloc_slice s) {
        s.retain();
        return {s.buf, s.size};
    }

    C4SliceResult sliceResult(slice s) {
        return sliceResult(alloc_slice(s));
    }

    C4SliceResult sliceResult(const char *str) {
        if (str)
            return sliceResult(slice{str, strlen(str)});
        else
            return {nullptr, 0};
    }

}


#pragma mark - LOGGING:

// LCOV_EXCL_START
void c4log_writeToCallback(C4LogLevel level, C4LogCallback callback, bool preformatted) noexcept {
    LogDomain::setCallback((LogDomain::Callback_t)callback, preformatted);
    LogDomain::setCallbackLogLevel((LogLevel)level);
}
// LCOV_EXCL_STOP

bool c4log_writeToBinaryFile(C4LogLevel level, C4String path, C4Error *outError) noexcept {
    return tryCatch(outError, [=] {
        LogDomain::writeEncodedLogsTo(slice(path).asString(), (LogLevel)level,
                                      string("Generated by LiteCore ") + getBuildInfo());
    });
}

C4LogLevel c4log_callbackLevel() noexcept        {return (C4LogLevel)LogDomain::callbackLogLevel();} // LCOV_EXCL_LINE
C4LogLevel c4log_binaryFileLevel() noexcept      {return (C4LogLevel)LogDomain::fileLogLevel();}

void c4log_setCallbackLevel(C4LogLevel level) noexcept   {LogDomain::setCallbackLogLevel((LogLevel)level);} //LCOV_EXCL_LINE
void c4log_setBinaryFileLevel(C4LogLevel level) noexcept {LogDomain::setFileLogLevel((LogLevel)level);}


CBL_CORE_API const C4LogDomain kC4DefaultLog    = (C4LogDomain)&kC4Cpp_DefaultLog;
CBL_CORE_API const C4LogDomain kC4DatabaseLog   = (C4LogDomain)&DBLog;
CBL_CORE_API const C4LogDomain kC4QueryLog      = (C4LogDomain)&QueryLog;
CBL_CORE_API const C4LogDomain kC4SyncLog       = (C4LogDomain)&SyncLog;
CBL_CORE_API const C4LogDomain kC4WebSocketLog  = (C4LogDomain)&websocket::WSLogDomain;


C4LogDomain c4log_getDomain(const char *name, bool create) noexcept {
    if (!name)
        return kC4DefaultLog;
    auto domain = LogDomain::named(name);
    if (!domain && create)
        domain = new LogDomain(name);
    return (C4LogDomain)domain;
}


const char* c4log_getDomainName(C4LogDomain c4Domain) noexcept {
    auto domain = (LogDomain*)c4Domain;
    return domain->name();
}


C4LogLevel c4log_getLevel(C4LogDomain c4Domain) noexcept {
    auto domain = (LogDomain*)c4Domain;
    return (C4LogLevel) domain->level();
}


void c4log_setLevel(C4LogDomain c4Domain, C4LogLevel level) noexcept {
    auto domain = (LogDomain*)c4Domain;
    domain->setLevel((LogLevel)level);
}


void c4log_warnOnErrors(bool warn) noexcept {
    error::sWarnOnError = warn;
}


void c4log(C4LogDomain c4Domain, C4LogLevel level, const char *fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    c4vlog(c4Domain, level, fmt, args);
    va_end(args);
}


void c4vlog(C4LogDomain c4Domain, C4LogLevel level, const char *fmt, va_list args) noexcept {
    try {
        ((LogDomain*)c4Domain)->vlog((LogLevel)level, fmt, args);
    } catch (...) { }
}

// LCOV_EXCL_START
void c4slog(C4LogDomain c4Domain, C4LogLevel level, C4Slice msg) noexcept {
    if(msg.buf == nullptr) {
        return;
    }
    
    c4log(c4Domain, level, "%.*s", SPLAT(msg));
}
// LCOV_EXCL_STOP

#pragma mark - INSTANCE COUNTED:


int c4_getObjectCount() noexcept {
    return gC4InstanceCount + websocket::WebSocket::gInstanceCount;
}

// LCOV_EXCL_START
#if DEBUG
static mutex sInstancesMutex;
static set<const C4InstanceCounted*> sInstances;

void C4InstanceCounted::track() const {
    lock_guard<mutex> lock(sInstancesMutex);
    sInstances.insert(this);
}

void C4InstanceCounted::untrack() const {
    lock_guard<mutex> lock(sInstancesMutex);
    sInstances.erase(this);
}

void c4_dumpInstances(void) C4API {
    char* unmangled = nullptr;
    lock_guard<mutex> lock(sInstancesMutex);
    for (const C4InstanceCounted *obj : sInstances) {
        const char *name =  typeid(*obj).name();
#ifdef __clang__
        int status;
        size_t unmangledLen = 0;
        unmangled = abi::__cxa_demangle(name, unmangled, &unmangledLen, &status);
        if (unmangled && status == 0)
            name = unmangled;
#endif
        fprintf(stderr, "    * %s at %p\n", name, obj);
    }
    free(unmangled);
}

#else
void c4_dumpInstances(void) C4API { }
#endif
// LCOV_EXCL_STOP
