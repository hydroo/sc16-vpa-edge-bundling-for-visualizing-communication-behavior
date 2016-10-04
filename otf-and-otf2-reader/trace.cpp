#include "trace.hpp"

timestamp_t Trace::beginTime() const {
    return _beginTime;
}

timestamp_t Trace::endTime() const {
    return _endTime;
}

const QSet<process_t>& Trace::processes() const {
    return _processes;
}

// note: determining this order is hackish for otf2, see rawtrace.cpp
const QList<process_t>& Trace::orderedProcesses() const {
    return _orderedProcesses;
}

const QMap<process_t, QString>& Trace::processNames() const {
    return _processNames;
}

const QList<Trace::Message>& Trace::messages(process_t p) const {
    if (_messages.contains(p)) {
        return _messages.constFind(p).value();
    } else {
        return _emptyMessageList;
    }
}
