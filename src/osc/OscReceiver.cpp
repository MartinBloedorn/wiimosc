#include "OscReceiver.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
const SocketHandle sInvalidSocket = INVALID_SOCKET;
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
const SocketHandle sInvalidSocket = -1;
#define closesocket close
#endif

namespace {

const int sMaxBundleDepth = 8;
/*! \brief How often the thread checks whether it should stop. */
const int sPollIntervalMs = 100;

/*! \brief Reads big-endian bytes, and the padded strings and blobs of OSC; any overrun marks it as failed. */
class Reader
{
public:
    Reader(const char *data, size_t size)
        : mData(reinterpret_cast<const uint8_t *>(data))
        , mSize(size)
    {
    }

    bool isAtEnd() const { return mOffset >= mSize; }
    bool hasFailed() const { return mHasFailed; }

    uint64_t readBigEndian(int bytes)
    {
        if (!take(bytes))
            return 0;
        uint64_t value = 0;
        for (int i = 0; i < bytes; ++i)
            value = (value << 8) | mData[mOffset - bytes + i];
        return value;
    }

    /*! \brief A null-terminated string, padded to a multiple of 4 bytes. */
    std::string readString()
    {
        const size_t start = mOffset;
        const void *end = mOffset < mSize ? std::memchr(mData + start, '\0', mSize - start) : nullptr;
        if (!end) {
            mHasFailed = true;
            return {};
        }
        const size_t length = static_cast<const uint8_t *>(end) - (mData + start);
        mOffset = start + ((length + 4) & ~size_t(3));
        if (mOffset > mSize)
            mHasFailed = true;
        return std::string(reinterpret_cast<const char *>(mData + start), length);
    }

    /*! \brief Skips \a bytes, padded to a multiple of 4. */
    void skip(size_t bytes) { take((bytes + 3) & ~size_t(3)); }

    const char *current() const { return reinterpret_cast<const char *>(mData + mOffset); }

private:
    bool take(size_t bytes)
    {
        if (mHasFailed || bytes > mSize - mOffset) {
            mHasFailed = true;
            return false;
        }
        mOffset += bytes;
        return true;
    }

    const uint8_t *mData;
    size_t mSize;
    size_t mOffset = 0;
    bool mHasFailed = false;
};

bool parseMessage(const char *data, size_t size, OscReceivedMessage *message)
{
    Reader reader(data, size);
    message->address = reader.readString();
    if (reader.hasFailed() || message->address.empty() || message->address[0] != '/')
        return false;
    if (reader.isAtEnd())
        return true; // no type tags: no arguments (pre-1.0 senders)
    const std::string typeTags = reader.readString();
    if (reader.hasFailed() || typeTags.empty() || typeTags[0] != ',')
        return false;

    for (size_t i = 1; i < typeTags.size(); ++i) {
        OscArgument argument;
        argument.type = typeTags[i];
        switch (argument.type) {
        case 'i':
            argument.number = static_cast<int32_t>(reader.readBigEndian(4));
            break;
        case 'f': {
            const uint32_t bits = static_cast<uint32_t>(reader.readBigEndian(4));
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            argument.number = value;
            break;
        }
        case 'h':
            argument.number = static_cast<double>(static_cast<int64_t>(reader.readBigEndian(8)));
            break;
        case 'd': {
            const uint64_t bits = reader.readBigEndian(8);
            std::memcpy(&argument.number, &bits, sizeof(argument.number));
            break;
        }
        case 's':
        case 'S':
            argument.string = reader.readString();
            break;
        case 'b':
            reader.skip(static_cast<uint32_t>(reader.readBigEndian(4)));
            break;
        case 't':
            reader.readBigEndian(8);
            break;
        case 'c':
        case 'r':
        case 'm':
            reader.readBigEndian(4);
            break;
        case 'T':
            argument.number = 1.0;
            break;
        case 'F':
        case 'N':
        case 'I':
            break;
        case '[':
        case ']':
            continue; // array delimiters: their elements are kept as plain arguments
        default:
            return false; // unknown size: the rest can't be read
        }
        if (reader.hasFailed())
            return false;
        message->arguments.push_back(argument);
    }
    return true;
}

bool parsePacket(const char *data, size_t size, std::vector<OscReceivedMessage> *messages, int depth)
{
    if (size == 0 || size % 4 != 0)
        return false;
    if (data[0] == '/') {
        OscReceivedMessage message;
        if (!parseMessage(data, size, &message))
            return false;
        messages->push_back(std::move(message));
        return true;
    }

    Reader reader(data, size);
    if (depth >= sMaxBundleDepth || reader.readString() != "#bundle")
        return false;
    reader.readBigEndian(8); // time tag
    while (!reader.hasFailed() && !reader.isAtEnd()) {
        const uint32_t elementSize = static_cast<uint32_t>(reader.readBigEndian(4));
        const char *element = reader.current();
        reader.skip(elementSize);
        if (reader.hasFailed() || !parsePacket(element, elementSize, messages, depth + 1))
            return false;
    }
    return !reader.hasFailed();
}

} // namespace

bool OscReceivedMessage::toFloat(size_t index, float *value) const
{
    if (index >= arguments.size())
        return false;
    switch (arguments[index].type) {
    case 'i':
    case 'f':
    case 'h':
    case 'd':
    case 'T':
    case 'F':
        *value = static_cast<float>(arguments[index].number);
        return true;
    default:
        return false;
    }
}

bool parseOscPacket(const char *data, size_t size, std::vector<OscReceivedMessage> *messages)
{
    return parsePacket(data, size, messages, 0);
}

struct OscReceiver::Private {
    SocketHandle socket = sInvalidSocket;
};

OscReceiver::OscReceiver()
    : d(new Private)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

OscReceiver::~OscReceiver()
{
    stop();
    if (d->socket != sInvalidSocket)
        closesocket(d->socket);
#ifdef _WIN32
    WSACleanup();
#endif
    delete d;
}

bool OscReceiver::open(uint16_t port)
{
    d->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d->socket == sInvalidSocket)
        return false;
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(d->socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        closesocket(d->socket);
        d->socket = sInvalidSocket;
        return false;
    }
    return true;
}

void OscReceiver::start(Handler handler)
{
    mHandler = std::move(handler);
    mIsRunning = true;
    mThread = std::thread(&OscReceiver::run, this);
}

void OscReceiver::stop()
{
    mIsRunning = false;
    if (mThread.joinable())
        mThread.join();
}

void OscReceiver::run()
{
    std::vector<char> buffer(65536); // the largest UDP payload
    std::vector<OscReceivedMessage> messages;
    while (mIsRunning) {
        // Waits with a timeout, so that stop() is noticed.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(d->socket, &readable);
        timeval timeout = {0, sPollIntervalMs * 1000};
        if (select(static_cast<int>(d->socket) + 1, &readable, nullptr, nullptr, &timeout) <= 0)
            continue;

        const int size = recvfrom(d->socket, buffer.data(), static_cast<int>(buffer.size()), 0, nullptr, nullptr);
        if (size <= 0)
            continue;
        messages.clear();
        if (!parseOscPacket(buffer.data(), static_cast<size_t>(size), &messages))
            std::printf("[osc] ignoring a malformed packet (%d bytes)\n", size);
        for (const OscReceivedMessage &message : messages)
            mHandler(message);
    }
}
