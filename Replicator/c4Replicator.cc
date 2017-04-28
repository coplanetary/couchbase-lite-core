//
//  c4Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "c4Socket+Internal.hh"
#include "c4ExceptionUtils.hh"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "StringUtil.hh"
#include <atomic>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


const char* const kC4ReplicatorActivityLevelNames[5] = {
    "stopped", "offline", "connecting", "idle", "busy"
};


static websocket::Address addressFrom(const C4Address &addr, C4String remoteDatabaseName) {
    return websocket::Address(asstring(addr.scheme),
                              asstring(addr.hostname),
                              addr.port,
                              format("/%.*s/_blipsync", SPLAT(remoteDatabaseName)));
}


static websocket::Address addressFrom(C4Database* otherDB) {
    alloc_slice path(c4db_getPath(otherDB));
    return websocket::Address("file", "", 0, path.asString());
}


struct C4Replicator : public RefCounted, Replicator::Delegate {

    // Constructor for replication with remote database
    C4Replicator(C4Database* db,
                 C4Address remoteAddress,
                 C4String remoteDatabaseName,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(db, DefaultProvider(), addressFrom(remoteAddress, remoteDatabaseName),
                  push, pull, onStateChanged, callbackContext)
    { }

    // Constructor for replication with local database
    C4Replicator(C4Database* db,
                 C4Database* otherDB,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(db, loopbackProvider(), addressFrom(otherDB),
                  push, pull, onStateChanged, callbackContext)
    {
        auto provider = loopbackProvider();
        auto dbAddr = addressFrom(db);
        _otherReplicator = new Replicator(otherDB,
                                          provider.createWebSocket(dbAddr), dbAddr,
                                          *this, { kC4Passive, kC4Passive });
        _otherLevel = _otherReplicator->status().level;
        provider.connect(_replicator->webSocket(), _otherReplicator->webSocket());
    }


    C4ReplicatorStatus status() const   {return _status;}

    void stop()                         {_replicator->stop();}

    void detach()                       {_onStateChanged = nullptr;}

private:

    C4Replicator(C4Database* db,
                 websocket::Provider &provider,
                 websocket::Address address,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :_onStateChanged(onStateChanged)
    ,_callbackContext(callbackContext)
    ,_replicator(new Replicator(db, provider, address, *this, { push, pull }))
    ,_status(_replicator->status())
    ,_selfRetain(this) // keep myself alive till replicator closes
    { }

    static LoopbackProvider& loopbackProvider() {
        static LoopbackProvider sProvider;
        return sProvider;
    }

    virtual void replicatorStatusChanged(Replicator *repl,
                                         const Replicator::Status &newStatus) override
    {
        if (repl == _replicator) {
            _status = newStatus;
            notify();
        } else if (repl == _otherReplicator) {
            _otherLevel = newStatus.level;
        }

        if (status().level == kC4Stopped && _otherLevel == kC4Stopped)
            _selfRetain = nullptr; // balances retain in constructor
    }

    void notify() {
        C4ReplicatorStatusChangedCallback on = _onStateChanged;
        if (on)
            on(this, _status, _callbackContext);
    }

    atomic<C4ReplicatorStatusChangedCallback> _onStateChanged;
    void *_callbackContext;
    Retained<Replicator> _replicator;
    Retained<Replicator> _otherReplicator;
    atomic<C4ReplicatorStatus> _status;
    C4ReplicatorActivityLevel _otherLevel {kC4Stopped};
    Retained<C4Replicator> _selfRetain;
};


static bool isValidScheme(C4Slice scheme) {
    static const slice kValidSchemes[] = {"ws"_sl, "wss"_sl, "blip"_sl, "blips"_sl};
    for (int i=0; i < sizeof(kValidSchemes)/sizeof(slice); i++)
        if (scheme == kValidSchemes[i])
            return true;
    return false;
}


bool c4repl_isValidDatabaseName(C4String dbName) {
    slice name = dbName;
    // Same rules as Couchbase Lite 1.x and CouchDB
    return name.size > 0 && name.size < 240
        && islower(name[0])
        && !slice(name).findByteNotIn("abcdefghijklmnopqrstuvwxyz0123456789_$()+-/"_sl);
}


bool c4repl_parseURL(C4String url, C4Address *address, C4String *dbName) {
    slice str = url;

    auto colon = str.findByteOrEnd(':');
    if (!colon)
        return false;
    address->scheme = slice(str.buf, colon);
    if (!isValidScheme(address->scheme))
        return false;
    address->port = (colon[-1] == 's') ? 443 : 80;
    str.setStart(colon);
    if (!str.hasPrefix("://"_sl))
        return false;
    str.moveStart(3);

    colon = str.findByteOrEnd(':');
    auto slash = str.findByteOrEnd('/');
    if (colon < slash) {
        int port;
        try {
            port = stoi(slice(colon+1, slash).asString());
        } catch (...) {
            return false;
        }
        if (port < 0 || port > 65535)
            return false;
        address->port = (uint16_t)port;
    } else {
        colon = slash;
    }
    address->hostname = slice(str.buf, colon);
    str.setStart(slash);
    if (str.size == 0)
        return false;
    address->path = "/"_sl;
    if (str[0] == '/')
        str.moveStart(1);
    if (str.hasSuffix("/"_sl))
        str.setSize(str.size - 1);
    *dbName = str;
    return c4repl_isValidDatabaseName(str);
}


C4Replicator* c4repl_new(C4Database* db,
                         C4Address serverAddress,
                         C4String remoteDatabaseName,
                         C4Database* otherLocalDB,
                         C4ReplicatorMode push,
                         C4ReplicatorMode pull,
                         C4ReplicatorStatusChangedCallback onStatusChanged,
                         void *callbackContext,
                         C4Error *outError) C4API
{
    try {
        if (!checkParam(push != kC4Disabled || pull != kC4Disabled,
                        "Either push or pull must be enabled", outError))
            return nullptr;

        c4::ref<C4Database> dbCopy(c4db_openAgain(db, outError));
        if (!dbCopy)
            return nullptr;

        C4Replicator *replicator;
        if (otherLocalDB) {
            if (!checkParam(otherLocalDB != db, "Can't replicate a database to itself", outError))
                return nullptr;
            // Local-to-local:
            c4::ref<C4Database> otherDBCopy(c4db_openAgain(otherLocalDB, outError));
            if (!otherDBCopy)
                return nullptr;
            replicator = new C4Replicator(dbCopy, otherDBCopy,
                                          push, pull, onStatusChanged, callbackContext);
        } else {
            // Remote:
            if (!checkParam(isValidScheme(serverAddress.scheme),
                            "Unsupported replication URL scheme", outError))
                return nullptr;
            replicator = new C4Replicator(dbCopy, serverAddress, remoteDatabaseName,
                                          push, pull, onStatusChanged, callbackContext);
        }
        return retain(replicator);
    } catchError(outError);
    return nullptr;
}


void c4repl_stop(C4Replicator* repl) C4API {
    repl->stop();
}


void c4repl_free(C4Replicator* repl) C4API {
    if (!repl)
        return;
    repl->stop();
    repl->detach();
    release(repl);
}


C4ReplicatorStatus c4repl_getStatus(C4Replicator *repl) C4API {
    return repl->status();
}
