#ifndef EDGE_BUNDLING_PROTOTYPE_PREREQS_HPP
#define EDGE_BUNDLING_PROTOTYPE_PREREQS_HPP

#include <cassert>
#include <stdint.h>

#include <functional>
#include <limits>

#include <QtCore>
#include <QtDebug>
#include <QtGlobal>
#include <QLoggingCategory>

using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
using f80 = long double;

template<typename T, typename S>
QTextStream& operator<<(QTextStream& s, const QMap<T, S>& m) {
    auto k = m.keys();
    s << "{";
    for(int i = 0; i < k.size(); i += 1) {
        s << k[i] << " : "<< m[k[i]];
        if (i < k.size()-1) { s << ", "; }
    }
    s << "}";
    return s;
}

class AutoFlushingQTextStream : public QTextStream {
public:
    AutoFlushingQTextStream(FILE* f, QIODevice::OpenMode o) : QTextStream(f, o) {}

    template<typename T>
    AutoFlushingQTextStream& operator<<(T&& s) {
        *((QTextStream*) this) << std::forward<T>(s);
        flush();
        return *this;
    }
};

// QTextStream always buffers, which is bad for debug output
extern AutoFlushingQTextStream qerr;
extern AutoFlushingQTextStream qout;

#endif // EDGE_BUNDLING_PROTOTYPE_PREREQS_HPP
