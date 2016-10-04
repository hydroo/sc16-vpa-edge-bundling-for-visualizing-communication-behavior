#ifndef EDGE_BUNDLING_PROTOTYPE_RAWTRACE_HPP
#define EDGE_BUNDLING_PROTOTYPE_RAWTRACE_HPP

#include "prereqs.hpp"

#include <otf2/otf2.h>

class Trace;

using process_t       = s64;
using processgroup_t  = s64;
using timestamp_t     = s64;
using messagetag_t    = s32;
using messagelength_t = s64;

class RawTrace {
public:
    struct SentMessage {
        timestamp_t     time;
        process_t       receiver;
        processgroup_t  group;
        messagelength_t length;
        messagetag_t    tag;
    };

    struct ReceivedMessage {
        timestamp_t     time;
        process_t       sender;
        processgroup_t  group;
        messagelength_t length;
        messagetag_t    tag;
    };

public:
    RawTrace() {}
    RawTrace(const RawTrace&) = delete;
    RawTrace(RawTrace&&)      = delete;

    RawTrace& operator=(const RawTrace&) = delete;
    RawTrace& operator=(RawTrace&&)      = delete;

    void setTraceFileName(const QString& f);

    void loadDefinitions();
    void loadEvents();
    void loadEvents(process_t p);

    timestamp_t beginTime() const; // needs loadEvents()
    timestamp_t endTime()   const; // needs loadEvents()

    const QSet<process_t>&            processes()      const; // needs loadDefinitions()
    const QMap<process_t, QString>&   processNames()   const; // needs loadDefinitions()
    const QMap<process_t, process_t>& processParents() const; // needs loadDefinitions()

    const QList<SentMessage>&     sentMessages(process_t p)     const; // needs loadEvents(p)
    const QList<ReceivedMessage>& receivedMessages(process_t p) const; // needs loadEvents(p)

    void toTrace(Trace* t); // needs loadDefinitions and loadEvents()

private:
    QString _traceFileName;

    bool _loadedDefinitions = false;
    QSet<process_t> _loadedEvents;

    timestamp_t _beginTime = std::numeric_limits<timestamp_t>::max();
    timestamp_t _endTime   = std::numeric_limits<timestamp_t>::min();

    QSet<process_t>                         _processes;
    QMap<process_t, QString>                _processNames;
    QMap<process_t, process_t>              _processParents;
    QMap<process_t, QList<SentMessage>>     _sentMessages;
    QMap<process_t, QList<ReceivedMessage>> _receivedMessages;

private:
    // used for otf2 local to global id mapping
    QMap<QPair<OTF2_CommRef, uint32_t /*local rank*/>, OTF2_LocationRef> _localRankToLocation;

    const QList<SentMessage>     _emptySentMessageList;
    const QList<ReceivedMessage> _emptyReceivedMessageList;

private:
    bool loadedAllEvents() const;
};

#endif // EDGE_BUNDLING_PROTOTYPE_RAWTRACE_HPP
