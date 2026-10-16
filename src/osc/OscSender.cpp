#include "OscSender.h"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
const SocketHandle sInvalidSocket = INVALID_SOCKET;
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
const SocketHandle sInvalidSocket = -1;
#define closesocket close
#endif

namespace {

/*! \brief Appends a null-terminated string, padded to a multiple of 4 bytes. */
void appendPadded(std::vector<char> &out, const std::string &str)
{
    out.insert(out.end(), str.begin(), str.end());
    const size_t padding = 4 - str.size() % 4;
    out.insert(out.end(), padding, '\0');
}

void appendBigEndian(std::vector<char> &out, uint32_t value)
{
    out.push_back(static_cast<char>(value >> 24));
    out.push_back(static_cast<char>(value >> 16));
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

} // namespace

OscMessage::OscMessage(const std::string &address)
    : mAddress(address)
{
}

OscMessage &OscMessage::add(int32_t value)
{
    mTypeTags += 'i';
    appendBigEndian(mArguments, static_cast<uint32_t>(value));
    return *this;
}

OscMessage &OscMessage::add(float value)
{
    mTypeTags += 'f';
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    appendBigEndian(mArguments, bits);
    return *this;
}

OscMessage &OscMessage::add(const std::string &value)
{
    mTypeTags += 's';
    appendPadded(mArguments, value);
    return *this;
}

std::vector<char> OscMessage::data() const
{
    std::vector<char> out;
    appendPadded(out, mAddress);
    appendPadded(out, mTypeTags);
    out.insert(out.end(), mArguments.begin(), mArguments.end());
    return out;
}

struct OscSender::Private {
    SocketHandle socket = sInvalidSocket;
    sockaddr_storage target = {};
    int targetLength = 0;
};

OscSender::OscSender()
    : d(new Private)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

OscSender::~OscSender()
{
    if (d->socket != sInvalidSocket)
        closesocket(d->socket);
#ifdef _WIN32
    WSACleanup();
#endif
    delete d;
}

bool OscSender::open(const std::string &host, uint16_t port)
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || !result)
        return false;
    std::memcpy(&d->target, result->ai_addr, result->ai_addrlen);
    d->targetLength = static_cast<int>(result->ai_addrlen);
    freeaddrinfo(result);

    d->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d->socket == sInvalidSocket)
        return false;
    // Allows broadcast targets like 255.255.255.255 or 192.168.1.255; no effect on unicast.
    const int isBroadcast = 1;
    setsockopt(d->socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&isBroadcast),
               sizeof(isBroadcast));
    return true;
}

bool OscSender::send(const OscMessage &message)
{
    const std::vector<char> data = message.data();
    return sendto(d->socket, data.data(), static_cast<int>(data.size()), 0,
                  reinterpret_cast<const sockaddr *>(&d->target), d->targetLength)
        == static_cast<int>(data.size());
}
