#include "rawtrace.hpp"

#include "trace.hpp"

using SentMessage     = RawTrace::SentMessage;
using ReceivedMessage = RawTrace::ReceivedMessage;

// otf specifics ////////////////////////////////////////////////////////////

#include <otf.h>

struct Otf {
    enum class Which { Unknown, Otf1, Otf2 } which;

    OTF_HandlerArray *h;
    OTF_FileManager *f;
    OTF_Reader *r;

    OTF2_GlobalDefReaderCallbacks *hd2;
    OTF2_GlobalEvtReaderCallbacks *he2;
    OTF2_Reader *r2;
};

static void Otf_init(Otf *otf);
static void Otf_open(const QString& traceFileName, Otf *otf);
static void Otf_finalize(Otf *otf);

static int handleDefProcess(void* userData, process_t id, const char* name, process_t parent);
static int handleSendMessage(void* userData, timestamp_t time, process_t sender, process_t receiver, processgroup_t group, messagetag_t tag, messagelength_t length);
static int handleReceiveMessage(void* userData, timestamp_t time, process_t receiver, process_t sender, processgroup_t group, messagetag_t tag, messagelength_t length);
static void handleEnterOrLeave(void* userData, timestamp_t time);

static int handleOtfDefProcess(void* userData, uint32_t stream, uint32_t id, const char* name, uint32_t parent);
static int handleOtfSendMessage(void* userData, uint64_t time, uint32_t sender, uint32_t receiver, uint32_t group, uint32_t tag, uint32_t length, uint32_t source, OTF_KeyValueList* list);
static int handleOtfReceiveMessage(void* userData, uint64_t time, uint32_t receiver, uint32_t sender, uint32_t group, uint32_t tag, uint32_t length, uint32_t source, OTF_KeyValueList* list);
static int handleOtfEnter(void* userData, uint64_t time, uint32_t id, uint32_t process, uint32_t source);
static int handleOtfLeave(void* userData, uint64_t time, uint32_t id, uint32_t process, uint32_t source);

static OTF2_CallbackCode handleOtf2DefProcess(void* userData, OTF2_LocationRef location, OTF2_StringRef name, OTF2_LocationType locationType, uint64_t numberOfEvents, OTF2_LocationGroupRef locationGroup);
static OTF2_CallbackCode handleOtf2DefString(void *userData, OTF2_StringRef self, const char *string);
static OTF2_CallbackCode handleOtf2DefGroup(void *userData, OTF2_GroupRef group, OTF2_StringRef name, OTF2_GroupType groupType, OTF2_Paradigm paradigm, OTF2_GroupFlag groupFlags, uint32_t numberOfMembers, const uint64_t *members);
static OTF2_CallbackCode handleOtf2DefCommunicator(void *userData, OTF2_CommRef com, OTF2_StringRef name, OTF2_GroupRef group, OTF2_CommRef parent);
static OTF2_CallbackCode handleOtf2MpiSend(OTF2_LocationRef sender, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, uint32_t receiver, OTF2_CommRef com, uint32_t tag, uint64_t length);
static OTF2_CallbackCode handleOtf2MpiIsend(OTF2_LocationRef sender, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint32_t receiver, OTF2_CommRef com, uint32_t tag, uint64_t length, uint64_t requestId);
static OTF2_CallbackCode handleOtf2MpiIsendComplete(OTF2_LocationRef sender, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint64_t requestId);
static OTF2_CallbackCode handleOtf2MpiRecv(OTF2_LocationRef receiver, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, uint32_t sender, OTF2_CommRef com, uint32_t tag, uint64_t length);
static OTF2_CallbackCode handleOtf2MpiIrecv(OTF2_LocationRef receiver, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint32_t sender, OTF2_CommRef com, uint32_t tag, uint64_t length, uint64_t requestId);
static OTF2_CallbackCode handleOtf2MpiIrecvRequest(OTF2_LocationRef locationId, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint64_t requestId);
static OTF2_CallbackCode handleOtf2MpiRequestCancelled(OTF2_LocationRef locationId, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint64_t requestID);
static OTF2_CallbackCode handleOtf2Enter(OTF2_LocationRef location, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, OTF2_RegionRef region);
static OTF2_CallbackCode handleOtf2Leave(OTF2_LocationRef location, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, OTF2_RegionRef region);

// RawTrace /////////////////////////////////////////////////////////////////
void RawTrace::setTraceFileName(const QString& f) {
    assert(_traceFileName      == QString());
    assert(_loadedDefinitions  == false);
    assert(_loadedEvents       == QSet<process_t>());

    _traceFileName = f;
}

struct DefinitionUserData {
    QSet<process_t>*              processes;
    QMap<process_t, QString>*     processNames;
    QMap<process_t, process_t>*   processParents;
    QMap<OTF2_StringRef, QString> strings;

    QMap<OTF2_CommRef, OTF2_GroupRef> communicatorToGroup;
    QMap<OTF2_GroupRef, QMap<uint32_t/*local rank*/, uint64_t /*rank in comm world*/>> localRankToGlobalRank;
    OTF2_GroupRef locationGroup;
    bool hasMpiLocationGroup = false;
};

void RawTrace::loadDefinitions() {
    assert(_traceFileName != QString());
    if (_loadedDefinitions == true) { return; }

    Otf otf;
    Otf_init(&otf);
    Otf_open(_traceFileName, &otf);

    DefinitionUserData u;
    u.processes      = &_processes;
    u.processNames   = &_processNames;
    u.processParents = &_processParents;

    if (otf.which == Otf::Which::Otf1) {
        OTF_HandlerArray_setHandler        (otf.h, (OTF_FunctionPointer*) handleOtfDefProcess, OTF_DEFPROCESS_RECORD);
        OTF_HandlerArray_setFirstHandlerArg(otf.h, &u                                        , OTF_DEFPROCESS_RECORD);

        OTF_Reader_readDefinitions(otf.r, otf.h);
    } else {
        OTF2_GlobalDefReaderCallbacks_SetLocationCallback(otf.hd2, &handleOtf2DefProcess     );
        OTF2_GlobalDefReaderCallbacks_SetStringCallback  (otf.hd2, &handleOtf2DefString      );
        OTF2_GlobalDefReaderCallbacks_SetGroupCallback   (otf.hd2, &handleOtf2DefGroup       );
        OTF2_GlobalDefReaderCallbacks_SetCommCallback    (otf.hd2, &handleOtf2DefCommunicator);

        OTF2_Reader_RegisterGlobalDefCallbacks(otf.r2, OTF2_Reader_GetGlobalDefReader(otf.r2), otf.hd2, &u);

        uint64_t dummyEventsRead;
        OTF2_Reader_ReadAllGlobalDefinitions(otf.r2, OTF2_Reader_GetGlobalDefReader(otf.r2), &dummyEventsRead);

        //set process names to include the group id
        foreach (process_t p, _processes) {
            unsigned long int stringId;
            unsigned int locationGroup;
            sscanf(_processNames[p].toStdString().c_str(),"%lu %u", &stringId, &locationGroup);
            _processNames[p] = QString("%1:%2").arg(u.strings[(OTF2_StringRef) stringId]).arg(locationGroup);
        }

        // generate _localRankToLocation from from communicatorToGroup, localRankToGlobalRank, globalRankToLocation
        // this might take too long -> change it maybe
        if (u.hasMpiLocationGroup == true) {

            QMapIterator<OTF2_CommRef, OTF2_GroupRef> i(u.communicatorToGroup);
            while (i.hasNext()) {
                i.next();
                auto com   = i.key();
                auto group = i.value();

                assert(u.localRankToGlobalRank.contains(group));
                QMapIterator<uint32_t, uint64_t> j(u.localRankToGlobalRank[group]);
                while (j.hasNext()) {
                    j.next();

                    auto localRank  = j.key();
                    auto globalRank = j.value();

                    if (u.localRankToGlobalRank[u.locationGroup].contains(globalRank)) {
                        auto location = u.localRankToGlobalRank[u.locationGroup][globalRank];
                        _localRankToLocation[QPair<OTF2_CommRef, uint32_t>(com, localRank)] = location;
                    } else {
                        // this is not an mpi rank. no mapping added. since threads do not send messages from their ids (hopefully)
                    }
                }
            }

            // make sure the child/parent ids gathered exist. This is not a given for our hackish OTF2 implementation.
            QMapIterator<process_t, process_t> k(_processParents);
            while (k.hasNext()) {
                k.next();
                assert(_processes.contains(k.key()));
                assert(_processes.contains(k.value()));
            }

        } else {
            // * this trace has no mpi calls
            // * don't create any mapping because the code would explode with 'u.locationGroup' being undefined
            // * assert(u.communicatorToGroup.isEmpty()); // doesn't work since there is something are communicators in traces without mpi. they refer to openmp groups e.g. (i don't know why. just reporting)
            // * this should be fine since _localRankToLocation is then empty and before every mapping there is an assertion for containment.
        }
    }

    Otf_finalize(&otf);

    _loadedDefinitions = true;
}

void RawTrace::loadEvents() {
    assert(_traceFileName != QString());

    loadDefinitions();

    foreach(auto p, _processes) {
        loadEvents(p);
    }
}

struct EventUserData {
    const QMap<QPair<OTF2_CommRef, uint32_t /*local rank*/>, OTF2_LocationRef>& localRankToLocation; // http://blog.automaton2000.com/2015/05/how-to-map-local-mpi-ranks-to-otf2-locations.html

    QMap<process_t, QList<SentMessage>>*     sentMessages;
    QMap<process_t, QList<ReceivedMessage>>* receivedMessages;
    timestamp_t* beginTime;
    timestamp_t* endTime;

    struct IreceiveRequest { // https://qvampir.zih.tu-dresden.de/Score-P-On/wiki/OTF2%20%3A%20How%20to%20Map%20Non-Blocking%20Send/Receive%20to%20normal%20Send/Receive
        uint64_t requestId;
        QQueue<ReceivedMessage> blockedReceives;
    };

    struct Isend {
        OTF2_TimeStamp     time;
        OTF2_LocationRef   sender;
        OTF2_LocationRef   receiver;
        OTF2_CommRef       com;
        uint64_t           length;
        uint32_t           tag;
        uint64_t           requestId;
        QQueue<SentMessage> blockedSends;
    };

    QMap<OTF2_LocationRef, QQueue<IreceiveRequest>> ireceiveRequests;
    QMap<OTF2_LocationRef, QQueue<Isend>>           isends;
};

void RawTrace::loadEvents(process_t p) {
    assert(_traceFileName != QString());
    if (_loadedEvents.contains(p)) { return; }

    _sentMessages[p]     = QList<SentMessage>    ();
    _receivedMessages[p] = QList<ReceivedMessage>();

    Otf otf;
    Otf_init(&otf);
    Otf_open(_traceFileName, &otf);

    EventUserData u{_localRankToLocation, &_sentMessages, &_receivedMessages, &_beginTime, &_endTime, QMap<OTF2_LocationRef, QQueue<EventUserData::IreceiveRequest>>{}, QMap<OTF2_LocationRef, QQueue<EventUserData::Isend>>{}};

    if (otf.which == Otf::Which::Otf1) {
        OTF_Reader_setProcessStatusAll(otf.r, 0);
        OTF_Reader_setProcessStatus(otf.r, p, 1);

        OTF_HandlerArray_setHandler        (otf.h, (OTF_FunctionPointer*) handleOtfSendMessage   , OTF_SEND_RECORD   );
        OTF_HandlerArray_setFirstHandlerArg(otf.h, &u                                            , OTF_SEND_RECORD   );
        OTF_HandlerArray_setHandler        (otf.h, (OTF_FunctionPointer*) handleOtfReceiveMessage, OTF_RECEIVE_RECORD);
        OTF_HandlerArray_setFirstHandlerArg(otf.h, &u                                            , OTF_RECEIVE_RECORD);
        OTF_HandlerArray_setHandler        (otf.h, (OTF_FunctionPointer*) handleOtfEnter         , OTF_ENTER_RECORD  );
        OTF_HandlerArray_setFirstHandlerArg(otf.h, &u                                            , OTF_ENTER_RECORD  );
        OTF_HandlerArray_setHandler        (otf.h, (OTF_FunctionPointer*) handleOtfLeave         , OTF_LEAVE_RECORD  );
        OTF_HandlerArray_setFirstHandlerArg(otf.h, &u                                            , OTF_LEAVE_RECORD  );

        OTF_Reader_readEvents(otf.r, otf.h);
    } else {
        OTF2_Reader_SelectLocation(otf.r2, p);

        bool successfullyOpenedDefinitions = OTF2_Reader_OpenDefFiles(otf.r2) == OTF2_SUCCESS;

        OTF2_Reader_OpenEvtFiles(otf.r2);

        if (successfullyOpenedDefinitions) { /* needed so otf2 can apply a mapping to map local id's to global ones */
            OTF2_DefReader* dr = OTF2_Reader_GetDefReader(otf.r2, p);
            if (dr != nullptr) {
                uint64_t dummy = 0;
                OTF2_Reader_ReadAllLocalDefinitions(otf.r2, dr, &dummy);
                OTF2_Reader_CloseDefReader(otf.r2, dr);
            }
        }
        OTF2_Reader_GetEvtReader(otf.r2, p); /* discard return value */

        if (successfullyOpenedDefinitions) { OTF2_Reader_CloseDefFiles(otf.r2); }

        OTF2_GlobalEvtReaderCallbacks_SetMpiSendCallback            (otf.he2, &handleOtf2MpiSend            );
        OTF2_GlobalEvtReaderCallbacks_SetMpiIsendCallback           (otf.he2, &handleOtf2MpiIsend           );
        OTF2_GlobalEvtReaderCallbacks_SetMpiIsendCompleteCallback   (otf.he2, &handleOtf2MpiIsendComplete   );
        OTF2_GlobalEvtReaderCallbacks_SetMpiRecvCallback            (otf.he2, &handleOtf2MpiRecv            );
        OTF2_GlobalEvtReaderCallbacks_SetMpiIrecvCallback           (otf.he2, &handleOtf2MpiIrecv           );
        OTF2_GlobalEvtReaderCallbacks_SetMpiIrecvRequestCallback    (otf.he2, &handleOtf2MpiIrecvRequest    );
        OTF2_GlobalEvtReaderCallbacks_SetMpiRequestCancelledCallback(otf.he2, &handleOtf2MpiRequestCancelled);
        OTF2_GlobalEvtReaderCallbacks_SetEnterCallback              (otf.he2, &handleOtf2Enter              );
        OTF2_GlobalEvtReaderCallbacks_SetLeaveCallback              (otf.he2, &handleOtf2Leave              );

        auto er = OTF2_Reader_GetGlobalEvtReader(otf.r2);

        OTF2_Reader_RegisterGlobalEvtCallbacks(otf.r2, er, otf.he2, &u);

        uint64_t dummyEventsRead;
        OTF2_Reader_ReadAllGlobalEvents(otf.r2, er, &dummyEventsRead);

        OTF2_Reader_CloseGlobalEvtReader(otf.r2, er);
        OTF2_Reader_CloseEvtFiles(otf.r2);
    }

    Otf_finalize(&otf);

    _loadedEvents.insert(p);
}

// latest enter/leave for otf and otf2
timestamp_t RawTrace::beginTime() const {
    assert(loadedAllEvents() == true);
    return _beginTime;
}

// latest enter/leave for otf and otf2
timestamp_t RawTrace::endTime() const {
    assert(loadedAllEvents() == true);
    return _endTime;
}

const QSet<process_t>& RawTrace::processes() const {
    assert(_loadedDefinitions == true);
    return _processes;
}

const QMap<process_t, QString>& RawTrace::processNames() const {
    assert(_loadedDefinitions == true);
    return _processNames;
}

// note: determining this order is hackish for OTF2
//
// In OTF2 a process is only a group which consists of equally valued locations. There is no hierarchie/parents as in otf.
// Vampir probably takes each group and defines its first (lowest id) element to be the group parent/leader process.
//
// In this implementation I use a bitmask to achieve the same, which works for score-p traces, but is not OTF2 compliant:
//   Every MPI Process has an id <= 0xffffffff.
//   Every non-Process has an ide > 0xffffffff.
//   The parent of such a non-Process is its id bitwise AND 0xffffffff.
//   Elements on the same level are ordered by id in ascending order.
//
// For otf parents exist and are used as intended.
const QMap<process_t, process_t>& RawTrace::processParents() const {
    assert(_loadedDefinitions == true);
    return _processParents;
}

const QList<RawTrace::SentMessage>& RawTrace::sentMessages(process_t p) const {
    assert(_loadedEvents.contains(p));
    if (_sentMessages.contains(p)) {
        return _sentMessages.constFind(p).value();
    } else {
        return _emptySentMessageList;
    }
}

const QList<RawTrace::ReceivedMessage>& RawTrace::receivedMessages(process_t p) const {
    assert(_loadedEvents.contains(p));
    if (_receivedMessages.contains(p)) {
        return _receivedMessages.constFind(p).value();
    } else {
        return _emptyReceivedMessageList;
    }
}

void RawTrace::toTrace(Trace* t) {
    assert(_loadedDefinitions == true);
    assert(loadedAllEvents() == true);

    assert(t->_beginTime == std::numeric_limits<timestamp_t>::max());
    assert(t->_endTime   == std::numeric_limits<timestamp_t>::min());
    assert(t->_processes       .isEmpty());
    assert(t->_orderedProcesses.isEmpty());
    assert(t->_processNames    .isEmpty());
    assert(t->_messages        .isEmpty());

    t->_beginTime    = _beginTime;
    t->_endTime      = _endTime;

    t->_processes    = _processes;
    t->_processNames = _processNames;

    QMap<process_t, QList<process_t>> children; // will be the reverse mapping of processParents

    QMapIterator<process_t, process_t> i(processParents());
    while (i.hasNext()) {
        i.next();
        children[i.value()].append(i.key());
    }

    // sort children lists
    QMutableMapIterator<process_t, QList<process_t>> j(children);
    while (j.hasNext()) {
        j.next();
        auto x = j.value();
        std::sort(j.value().begin(), j.value().end());
    }

    auto sortedProcesses = processes().toList(); // QSet is unordered. We need them ordered.
    std::sort(sortedProcesses.begin(), sortedProcesses.end());

    QSet<process_t> added;

    // Add process ids to orderedProcesses recursively. Children might be parents of other processes themselves.
    std::function<void(process_t)> recurse = [&recurse, &sortedProcesses, &children, &added, &t](process_t parent) {
        foreach (process_t p, sortedProcesses) {
            if (added.contains(p) || children.contains(parent) == false) { continue; }
            if (children[parent].contains(p) == false) { continue; }
            t->_orderedProcesses.append(p);
            added.insert(p);

            recurse(p);
        }
    };

    foreach (process_t p, sortedProcesses) {
        if (added.contains(p)) { continue; }
        t->_orderedProcesses.append(p);
        added.insert(p);

        recurse(p);
    }

    // match messages. I.e. transform send/recvs into to Trace::Message structures.

    struct Key {
        process_t      sender;
        process_t      receiver;
        processgroup_t group;
        messagetag_t   tag;
        bool operator<(const Key& o) const {
            if      (sender   < o.sender  ) { return true ; }
            else if (sender   > o.sender  ) { return false; }
            if      (receiver < o.receiver) { return true ; }
            else if (receiver > o.receiver) { return false; }
            if      (group    < o.group   ) { return true;  }
            else if (group    > o.group   ) { return false; }
            return tag < o.tag;
        }
        QString toString() const {
            QString ret;
            QTextStream s(&ret);
            s << "sender " << sender << ", receiver " << receiver << ", group " << group << ", tag " << tag;
            return ret;
        }
    };

    QMap<Key, QList<ReceivedMessage>> receiveQueues;

    foreach (process_t receiver, processes()) {
        foreach (const auto& r, receivedMessages(receiver)) {
            receiveQueues[Key{r.sender, receiver, r.group, r.tag}].append(r);
        }
    }

    QMap<Key, int /*count*/> missingReceives;

    foreach (process_t sender, processes()) {

        t->_messages[sender] = {};

        foreach (const auto& s, sentMessages(sender)) {
            auto k = Key{sender, s.receiver, s.group, s.tag};

            if (receiveQueues.contains(k)) {
                const auto& r = receiveQueues[k].first();

                t->_messages[sender].append(Trace::Message{s.time, r.time - s.time, s.receiver, s.length});

                if (s.time > r.time) {
                    qerr << "warning: send (process " << sender << ") did not start before receive (process " << s.receiver << "). delta is " << r.time - s.time << " ticks.\n";
                }
                if (s.length > r.length) {
                    qerr << "warning: receiver (process " << s.receiver << ") receives fewer bytes than sent (process " << sender << "). " << s.length << " > " << r.length << "\n";
                }

                receiveQueues[k].removeFirst();

                if (receiveQueues[k].isEmpty()) {
                    receiveQueues.remove(k);
                }

            } else {
                if (missingReceives.contains(k) == false) { missingReceives[k] = 0; }
                missingReceives[k] += 1;
            }
        }
    }

    if (missingReceives.isEmpty() == false) { // there exists sends without receives
        QMapIterator<Key, int> i(missingReceives);
        while (i.hasNext()) {
            i.next();
            qerr << "warning: key: \""<< i.key().toString() << "\" has " << i.value() << " missing receives.\n";
        }
    }

    assert(receiveQueues.isEmpty()); // if this happens, receives are done without according sends. To my best knowledge this is illegal.
}

bool RawTrace::loadedAllEvents() const {
    assert(_loadedDefinitions == true);
    return _loadedEvents.size() == _sentMessages.size();
}

// otf specifics ////////////////////////////////////////////////////////////

static void Otf_init(Otf *otf) {
    otf->which = Otf::Which::Unknown;

    otf->f = OTF_FileManager_open(900);
    assert(otf->f != nullptr);
    otf->h = OTF_HandlerArray_open();
    assert(otf->h != nullptr);
    otf->r = nullptr;

    otf->hd2 = OTF2_GlobalDefReaderCallbacks_New();
    otf->he2 = OTF2_GlobalEvtReaderCallbacks_New();
    otf->r2  = nullptr;
}

static void Otf_open(const QString& traceFileName, Otf *otf) {
    assert(otf->which == Otf::Which::Unknown); /* make sure nothing has been opened already */

    otf->r = OTF_Reader_open(traceFileName.toStdString().c_str(), otf->f);
    if (otf->r == nullptr) {
        otf->r2 = OTF2_Reader_Open(traceFileName.toStdString().c_str());
        if (otf->r2 == nullptr) {
            qout << "could not open \"" << traceFileName << "\". aborting.\n";
            exit(-1);
        } else {
            otf->which = Otf::Which::Otf2;
        }
    } else {
        otf->which = Otf::Which::Otf1;
    }
}

static void Otf_finalize(Otf *otf) {
    if (otf->r != nullptr) { OTF_Reader_close(otf->r);       otf->r = nullptr; }
    if (otf->h != nullptr) { OTF_HandlerArray_close(otf->h); otf->h = nullptr; }
    if (otf->f != nullptr) { OTF_FileManager_close(otf->f);  otf->f = nullptr; }

    if (otf->r2  != nullptr) { OTF2_Reader_Close(otf->r2);                     otf->r2  = nullptr; }
    if (otf->he2 != nullptr) { OTF2_GlobalEvtReaderCallbacks_Delete(otf->he2); otf->he2 = nullptr; }
    if (otf->hd2 != nullptr) { OTF2_GlobalDefReaderCallbacks_Delete(otf->hd2); otf->hd2 = nullptr; }
}

// unified handlers /////////////////////////////////////////////////////////

static int handleDefProcess(void* userData, process_t id, const char* name, process_t parent) {
    auto& u = *((DefinitionUserData*) userData);
    if (u.processNames->contains(id)) {
        qerr << "process " << id << " \"" << name << "\" has already been defined. aborting.\n";
        assert(false);
    } else {
        (*u.processNames)[id] = QString("%1").arg(name).trimmed();
        u.processes->insert(id);
        if (parent != -1) { u.processParents->insert(id, parent); }
    }
    return OTF_RETURN_OK;
}

static int handleSendMessage(void* userData, timestamp_t time, process_t sender, process_t receiver, processgroup_t group, messagetag_t tag, messagelength_t length) {
    (void) sender; // is hardcoded into userData
    (*((EventUserData*) userData)->sentMessages)[sender].append(SentMessage{time, receiver, group, length, tag});
    return OTF_RETURN_OK;
}
static int handleReceiveMessage(void* userData, timestamp_t time, process_t receiver, process_t sender, processgroup_t group, messagetag_t tag, messagelength_t length) {
    (void) receiver; // is hardcoded into userData
    (*((EventUserData*) userData)->receivedMessages)[receiver].append(ReceivedMessage{time, sender, group, length, tag});
    return OTF_RETURN_OK;
}

static void handleEnterOrLeave(void* userData, timestamp_t time) {
    auto& u = *(EventUserData*) userData;
    *u.beginTime = std::min(*u.beginTime, time);
    *u.endTime   = std::max(*u.endTime  , time);
}

// otf handlers /////////////////////////////////////////////////////////////

static int handleOtfDefProcess(void* userData, uint32_t stream, uint32_t id, const char* name, uint32_t parent) {
    (void) stream; (void) parent;
    return handleDefProcess(userData, (process_t) id, name, parent == 0 ? (process_t) -1 : (process_t) parent);
}

static int handleOtfSendMessage(void* userData, uint64_t time, uint32_t sender, uint32_t receiver, uint32_t group, uint32_t tag, uint32_t length, uint32_t source, OTF_KeyValueList* list) {
    (void) source; (void) list;
    return handleSendMessage(userData, (timestamp_t) time, (process_t) sender, (process_t) receiver, (processgroup_t) group, (messagetag_t) tag, (messagelength_t) length);
}

static int handleOtfReceiveMessage(void* userData, uint64_t time, uint32_t receiver, uint32_t sender, uint32_t group, uint32_t tag, uint32_t length, uint32_t source, OTF_KeyValueList* list) {
    (void) source; (void) list;
    return handleReceiveMessage(userData, (timestamp_t) time, (process_t) receiver, (process_t) sender, (processgroup_t) group, (messagetag_t) tag, (messagelength_t) length);
}

static int handleOtfEnter(void* userData, uint64_t time, uint32_t function, uint32_t process, uint32_t source) {
    (void) function; (void) process; (void) source;
    handleEnterOrLeave(userData, time);
    return OTF_RETURN_OK;
}

static int handleOtfLeave(void* userData, uint64_t time, uint32_t function, uint32_t process, uint32_t source) {
    (void) function; (void) process; (void) source;
    handleEnterOrLeave(userData, time);
    return OTF_RETURN_OK;
}

// otf 2 handlers ///////////////////////////////////////////////////////////

/* note: string ref needs to be resolved later */
static OTF2_CallbackCode handleOtf2DefProcess(void* userData, OTF2_LocationRef location, OTF2_StringRef name, OTF2_LocationType locationType, uint64_t numberOfEvents, OTF2_LocationGroupRef locationGroup) {
    (void) numberOfEvents; (void) locationGroup;

    if (locationType == OTF2_LOCATION_TYPE_METRIC) { return OTF2_CALLBACK_SUCCESS; }

    process_t parent;
    if ((location & 0xffffffff) == location) { parent = -1; }
    else                                     { parent = (process_t) (location & 0xffffffff); }

    if (handleDefProcess(userData, (process_t) location, QString("%1 %2").arg((uint32_t)name).arg(locationGroup).toStdString().c_str(), parent) == OTF_RETURN_OK) {
        return OTF2_CALLBACK_SUCCESS;
    } else {
        return OTF2_CALLBACK_ERROR;
    }
}

static OTF2_CallbackCode handleOtf2DefString(void *userData, OTF2_StringRef self, const char *string) {
    auto& strings = ((DefinitionUserData*) userData)->strings;
    if (strings.contains(self)) {
        qerr << "string " << self << " \"" << string << "\" has already been defined. aborting.\n";
        assert(false);
    } else {
        strings[self] = QString("%1").arg(string);
    }
    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2DefGroup(void *userData, OTF2_GroupRef group, OTF2_StringRef name, OTF2_GroupType type, OTF2_Paradigm paradigm, OTF2_GroupFlag groupFlags, uint32_t numberOfMembers, const uint64_t *members) {
    (void) name; (void) paradigm; (void) groupFlags;

    auto& u = *((DefinitionUserData*) userData);

    assert(u.localRankToGlobalRank.contains(group) == false);
    u.localRankToGlobalRank[group] = {};
    //qerr << "group" << group << ": [";
    for (uint32_t i = 0; i < numberOfMembers; i += 1) {
        u.localRankToGlobalRank[group][i] = members[i];
        //qerr << members[i] << ", ";
    }
    //qerr << "] type " << type << ", paradigm " << paradigm << "\n";

    if (type == OTF2_GROUP_TYPE_COMM_LOCATIONS && paradigm == OTF2_PARADIGM_MPI) {
        u.locationGroup = group;
        assert(u.hasMpiLocationGroup == false);
        u.hasMpiLocationGroup = true;
    }
    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2DefCommunicator(void *userData, OTF2_CommRef com, OTF2_StringRef name, OTF2_GroupRef group, OTF2_CommRef parent) {
    (void) name; (void) parent;
    ((DefinitionUserData*) userData)->communicatorToGroup[com] = group;
    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2MpiSend(OTF2_LocationRef sender, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, uint32_t localReceiverRank, OTF2_CommRef com, uint32_t tag, uint64_t length) {
    (void) a;
    auto& u = *((EventUserData*) userData);

    assert(u.localRankToLocation.contains(QPair<OTF2_CommRef, uint32_t>(com, localReceiverRank)));
    auto receiver = u.localRankToLocation[QPair<OTF2_CommRef, uint32_t>(com, localReceiverRank)];

    if (u.isends[sender].isEmpty()) {
        if (handleSendMessage(userData, (timestamp_t) time, (process_t) sender, (process_t) receiver, (processgroup_t) com, (messagetag_t) tag, (messagelength_t) length) == OTF_RETURN_OK) {
            return OTF2_CALLBACK_SUCCESS;
        } else {
            return OTF2_CALLBACK_ERROR;
        }
    } else { // for correct send/recv matching we need to withhold this send until the previously issued isends are done
        u.isends[sender].last().blockedSends.enqueue(SentMessage{(timestamp_t) time, (process_t) receiver, (processgroup_t) com, (messagelength_t) length, (messagetag_t) tag});
        return OTF2_CALLBACK_SUCCESS;
    }
}

static OTF2_CallbackCode handleOtf2MpiIsend(OTF2_LocationRef sender, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint32_t localReceiverRank, OTF2_CommRef com, uint32_t tag, uint64_t length, uint64_t requestId) {
    (void) a;
    auto& u = *((EventUserData*) userData);

    assert(u.localRankToLocation.contains(QPair<OTF2_CommRef, uint32_t>(com, localReceiverRank)));
    auto receiver = u.localRankToLocation[QPair<OTF2_CommRef, uint32_t>(com, localReceiverRank)];

    u.isends[sender].enqueue(EventUserData::Isend{time, sender, receiver, com, length, tag, requestId, QQueue<SentMessage>{}});

    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2MpiIsendComplete(OTF2_LocationRef sender, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint64_t requestId) {
    (void) time; (void) a;
    auto& u = *((EventUserData*) userData);

    auto& isends = u.isends[sender];

    // find the matching isend
    int index = -1;
    for (int i = 0; i < isends.size(); i += 1) {
        if (isends[i].requestId == requestId) {
            index = i;
            break;
        }
    }
    assert(index != -1);

    const auto& s = isends[index];

    if (index == 0) {
        if (handleSendMessage(userData, (timestamp_t) s.time, (process_t) sender, (process_t) s.receiver, (processgroup_t) s.com, (messagetag_t) s.tag, (messagelength_t) s.length) != OTF_RETURN_OK) {
            return OTF2_CALLBACK_ERROR;
        }

        foreach (const auto& b, s.blockedSends) {
            if (handleSendMessage(userData, b.time, (process_t) sender, b.receiver, b.group, b.tag, b.length) != OTF_RETURN_OK) {
                return OTF2_CALLBACK_ERROR;
            }
        }
    } else {
        isends[index-1].blockedSends.enqueue(SentMessage{(timestamp_t) s.time, (process_t) s.receiver, (processgroup_t) s.com, (messagelength_t) s.length, (messagetag_t) s.tag});

        isends[index-1].blockedSends.append(s.blockedSends);
    }

    isends.removeAt(index);

    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2MpiRecv(OTF2_LocationRef receiver, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, uint32_t localSenderRank, OTF2_CommRef com, uint32_t tag, uint64_t length) {
    (void) a;
    auto& u = *((EventUserData*) userData);

    assert(u.localRankToLocation.contains(QPair<OTF2_CommRef, uint32_t>(com, localSenderRank)));
    auto sender = u.localRankToLocation[QPair<OTF2_CommRef, uint32_t>(com, localSenderRank)];

    if (u.ireceiveRequests[receiver].isEmpty()) {
        if (handleReceiveMessage(userData, (timestamp_t) time, (process_t) receiver, (process_t) sender, (processgroup_t) com, (messagetag_t) tag, (messagelength_t) length) == OTF_RETURN_OK) {
            return OTF2_CALLBACK_SUCCESS;
        } else {
            return OTF2_CALLBACK_ERROR;
        }
    } else {
        u.ireceiveRequests[receiver].last().blockedReceives.enqueue(ReceivedMessage{(timestamp_t) time, (process_t) sender, (processgroup_t) com, (messagelength_t) length, (messagetag_t) tag});
        return OTF2_CALLBACK_SUCCESS;
    }
}

static OTF2_CallbackCode handleOtf2MpiIrecv(OTF2_LocationRef receiver, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint32_t sender, OTF2_CommRef com, uint32_t tag, uint64_t length, uint64_t requestId) {
    (void) a;

    auto& u = *((EventUserData*) userData);

    auto& ireceiveRequests = u.ireceiveRequests[receiver];

    // find the matching ireceive request
    int index = -1;
    for (int i = 0; i < ireceiveRequests.size(); i += 1) {
        if (ireceiveRequests[i].requestId == requestId) {
            index = i;
            break;
        }
    }
    assert(index != -1);

    const auto& r = ireceiveRequests[index];

    if (index == 0) {
        if (handleReceiveMessage(userData, (timestamp_t) time, (process_t) receiver, (process_t) sender, (processgroup_t) com, (messagetag_t) tag, (messagelength_t) length) != OTF_RETURN_OK) {
            return OTF2_CALLBACK_ERROR;
        }

        foreach (const auto& b, r.blockedReceives) {
            if (handleReceiveMessage(userData, b.time, (process_t) receiver, b.sender, b.group, b.tag, b.length) != OTF_RETURN_OK) {
                return OTF2_CALLBACK_ERROR;
            }
        }
    } else {
        ireceiveRequests[index-1].blockedReceives.enqueue(ReceivedMessage{(timestamp_t) time, (process_t) sender, (processgroup_t) com, (messagelength_t) length, (messagetag_t) tag});

        ireceiveRequests[index-1].blockedReceives.append(r.blockedReceives);
    }

    ireceiveRequests.removeAt(index);

    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2MpiIrecvRequest(OTF2_LocationRef receiver, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint64_t requestId) {
    (void) time; (void) a;
    auto& u = *((EventUserData*) userData);
    u.ireceiveRequests[receiver].enqueue(EventUserData::IreceiveRequest{requestId, QQueue<ReceivedMessage>{}});
    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2MpiRequestCancelled(OTF2_LocationRef locationId, OTF2_TimeStamp time, void *userData, OTF2_AttributeList *a, uint64_t requestId) {
    (void) time; (void) a;

    auto& u = *((EventUserData*) userData);

    auto& ireceiveRequests = u.ireceiveRequests[locationId];
    auto& isends           = u.isends[locationId];

    // find the matching ireceive request
    int ireceiveRequestIndex = -1;
    for (int i = 0; i < ireceiveRequests.size(); i += 1) {
        if (ireceiveRequests[i].requestId == requestId) {
            ireceiveRequestIndex = i;
            break;
        }
    }

    // find the matching isend
    int isendIndex = -1;
    for (int i = 0; i < isends.size(); i += 1) {
        if (isends[i].requestId == requestId) {
            isendIndex = i;
            break;
        }
    }

    assert((ireceiveRequestIndex != -1 && isendIndex != -1) == false);

    if (ireceiveRequestIndex != -1) { // like handleOtf2MpiIrecv without recording a new ireceive

        if (ireceiveRequestIndex == 0) {
            foreach (const auto& b, ireceiveRequests[ireceiveRequestIndex].blockedReceives) {
                if (handleReceiveMessage(userData, b.time, (process_t) locationId, b.sender, b.group, b.tag, b.length) != OTF_RETURN_OK) {
                    return OTF2_CALLBACK_ERROR;
                }
            }
        } else {
            ireceiveRequests[ireceiveRequestIndex-1].blockedReceives.append(ireceiveRequests[ireceiveRequestIndex].blockedReceives);
        }

        ireceiveRequests.removeAt(ireceiveRequestIndex);

    } else if (isendIndex != -1) { // like handleOtf2MpiIsendComplete without record a new isend

        if (isendIndex == 0) {
            foreach (const auto& b, isends[isendIndex].blockedSends) {
                if (handleSendMessage(userData, b.time, (process_t) locationId, b.receiver, b.group, b.tag, b.length) != OTF_RETURN_OK) {
                    return OTF2_CALLBACK_ERROR;
                }
            }

        } else {
            isends[isendIndex-1].blockedSends.append(isends[isendIndex].blockedSends);
        }

        isends.removeAt(isendIndex);

    } else { //neither ireceive request nor isend got cancelled
    }

    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2Enter(OTF2_LocationRef location, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, OTF2_RegionRef region) {
    (void) location; (void) a; (void) region;
    handleEnterOrLeave(userData, time);
    return OTF2_CALLBACK_SUCCESS;
}

static OTF2_CallbackCode handleOtf2Leave(OTF2_LocationRef location, OTF2_TimeStamp time, void* userData, OTF2_AttributeList* a, OTF2_RegionRef region) {
    (void) location; (void) a; (void) region;
    handleEnterOrLeave(userData, time);
    return OTF2_CALLBACK_SUCCESS;
}
