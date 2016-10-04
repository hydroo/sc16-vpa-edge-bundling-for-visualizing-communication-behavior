#ifndef EDGE_BUNDLING_PROTOTYPE_TRACE_HPP
#define EDGE_BUNDLING_PROTOTYPE_TRACE_HPP

#include "prereqs.hpp"

#include "rawtrace.hpp"

class Trace {
public:
    struct Message {
        timestamp_t     time;
        timestamp_t     duration;
        process_t       receiver;
        messagelength_t length; // only sender size counts
    };

public:
    Trace() {};
    Trace(const Trace&) = delete;
    Trace(Trace&&)      = delete;

    Trace& operator=(const Trace&) = delete;
    Trace& operator=(Trace&&)      = delete;

    timestamp_t beginTime() const;
    timestamp_t endTime()   const;

    const QSet<process_t>&          processes()        const;
    const QList<process_t>&         orderedProcesses() const; // order of the Vampir Master Timeline
    const QMap<process_t, QString>& processNames()     const;

    const QList<Message>& messages(process_t p) const;

private:
    timestamp_t _beginTime = std::numeric_limits<timestamp_t>::max();
    timestamp_t _endTime   = std::numeric_limits<timestamp_t>::min();

    QSet<process_t>                            _processes;
    QList<process_t>                           _orderedProcesses;
    QMap<process_t, QString>                   _processNames;
    QMap<process_t /*sender*/, QList<Message>> _messages;

private:
    const QList<Message> _emptyMessageList;

    friend RawTrace;
};

#endif // EDGE_BUNDLING_PROTOTYPE_TRACE_HPP
