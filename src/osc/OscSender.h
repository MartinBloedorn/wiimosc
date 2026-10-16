#pragma once

#include <cstdint>
#include <string>
#include <vector>

/*! \brief An OSC 1.0 message with int32, float32 and string arguments. */
class OscMessage
{
public:
    explicit OscMessage(const std::string &address);

    OscMessage &add(int32_t value);
    OscMessage &add(float value);
    OscMessage &add(const std::string &value);

    /*! \brief Encodes the message into its wire format. */
    std::vector<char> data() const;

private:
    std::string mAddress;
    std::string mTypeTags = ",";
    std::vector<char> mArguments;
};

/*! \brief Sends OSC messages over UDP, to a unicast or broadcast address. */
class OscSender
{
public:
    OscSender();
    ~OscSender();

    OscSender(const OscSender &) = delete;
    OscSender &operator=(const OscSender &) = delete;

    /*! \brief Resolves \a host (name or IPv4 address) and creates the socket. */
    bool open(const std::string &host, uint16_t port);

    bool send(const OscMessage &message);

private:
    struct Private;
    Private *d;
};
