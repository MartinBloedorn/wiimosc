#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

/*! \brief A received OSC argument. Numbers (i, f, h, d) and booleans (T, F) are kept in \c number. */
struct OscArgument {
    char type = 0;
    double number = 0.0;
    std::string string; //!< s, S
};

/*! \brief A received OSC message. */
struct OscReceivedMessage {
    std::string address;
    std::vector<OscArgument> arguments;

    /*! \brief Reads argument \a index as a number (i, f, h, d, T, F); false if it's missing or not a number. */
    bool toFloat(size_t index, float *value) const;
};

/*! \brief Parses an OSC 1.0 packet, a message or a bundle, and appends its messages to \a messages.

    Bundle elements are appended in order; their time tags are ignored (everything is "immediately"). Returns false
    if the packet is malformed, in which case \a messages may hold the messages parsed before the error.
 */
bool parseOscPacket(const char *data, size_t size, std::vector<OscReceivedMessage> *messages);

/*! \brief Receives OSC messages over UDP, on its own thread. */
class OscReceiver
{
public:
    /*! \brief Called on the receiver's thread, for each message. */
    using Handler = std::function<void(const OscReceivedMessage &)>;

    OscReceiver();
    ~OscReceiver();

    OscReceiver(const OscReceiver &) = delete;
    OscReceiver &operator=(const OscReceiver &) = delete;

    /*! \brief Binds the socket to \a port on all interfaces. */
    bool open(uint16_t port);

    /*! \brief Starts the thread, which passes each received message to \a handler. */
    void start(Handler handler);
    void stop();

private:
    void run();

    struct Private;
    Private *d;
    Handler mHandler;
    std::thread mThread;
    std::atomic<bool> mIsRunning{false};
};
