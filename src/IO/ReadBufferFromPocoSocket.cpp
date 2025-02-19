#include <Poco/Net/NetException.h>

#include <base/scope_guard.h>

#include <IO/ReadBufferFromPocoSocket.h>
#include <Common/Exception.h>
#include <Common/NetException.h>
#include <Common/Stopwatch.h>
#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>
#include <Common/AsyncTaskExecutor.h>

namespace ProfileEvents
{
    extern const Event NetworkReceiveElapsedMicroseconds;
    extern const Event NetworkReceiveBytes;
}

namespace CurrentMetrics
{
    extern const Metric NetworkReceive;
}


namespace DB
{
namespace ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int SOCKET_TIMEOUT;
    extern const int CANNOT_READ_FROM_SOCKET;
    extern const int LOGICAL_ERROR;
}


bool ReadBufferFromPocoSocket::nextImpl()
{
    ssize_t bytes_read = 0;
    Stopwatch watch;

    SCOPE_EXIT({
        // / NOTE: it is quite inaccurate on high loads since the thread could be replaced by another one
        ProfileEvents::increment(ProfileEvents::NetworkReceiveElapsedMicroseconds, watch.elapsedMicroseconds());
        ProfileEvents::increment(ProfileEvents::NetworkReceiveBytes, bytes_read);
    });

    /// Add more details to exceptions.
    try
    {
        CurrentMetrics::Increment metric_increment(CurrentMetrics::NetworkReceive);

        /// If async_callback is specified, and read will block, run async_callback and try again later.
        /// It is expected that file descriptor may be polled externally.
        /// Note that receive timeout is not checked here. External code should check it while polling.
        while (async_callback && !socket.poll(0, Poco::Net::Socket::SELECT_READ | Poco::Net::Socket::SELECT_ERROR))
            async_callback(socket.impl()->sockfd(), socket.getReceiveTimeout(), AsyncEventTimeoutType::RECEIVE, socket_description, AsyncTaskExecutor::Event::READ | AsyncTaskExecutor::Event::ERROR);

        if (internal_buffer.size() > INT_MAX)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Buffer overflow");

        bytes_read = socket.impl()->receiveBytes(internal_buffer.begin(), static_cast<int>(internal_buffer.size()));
    }
    catch (const Poco::Net::NetException & e)
    {
        throw NetException(ErrorCodes::NETWORK_ERROR, "{}, while reading from socket ({})", e.displayText(), peer_address.toString());
    }
    catch (const Poco::TimeoutException &)
    {
        throw NetException(ErrorCodes::SOCKET_TIMEOUT, "Timeout exceeded while reading from socket ({}, {} ms)",
            peer_address.toString(),
            socket.impl()->getReceiveTimeout().totalMilliseconds());
    }
    catch (const Poco::IOException & e)
    {
        throw NetException(ErrorCodes::NETWORK_ERROR, "{}, while reading from socket ({})", e.displayText(), peer_address.toString());
    }

    if (bytes_read < 0)
        throw NetException(ErrorCodes::CANNOT_READ_FROM_SOCKET, "Cannot read from socket ({})", peer_address.toString());

    if (bytes_read)
        working_buffer.resize(bytes_read);
    else
        return false;

    return true;
}

ReadBufferFromPocoSocket::ReadBufferFromPocoSocket(Poco::Net::Socket & socket_, size_t buf_size)
    : BufferWithOwnMemory<ReadBuffer>(buf_size)
    , socket(socket_)
    , peer_address(socket.peerAddress())
    , socket_description("socket (" + peer_address.toString() + ")")
{
}

bool ReadBufferFromPocoSocket::poll(size_t timeout_microseconds) const
{
    if (available())
        return true;

    Stopwatch watch;
    bool res = socket.poll(timeout_microseconds, Poco::Net::Socket::SELECT_READ | Poco::Net::Socket::SELECT_ERROR);
    ProfileEvents::increment(ProfileEvents::NetworkReceiveElapsedMicroseconds, watch.elapsedMicroseconds());
    return res;
}

}
