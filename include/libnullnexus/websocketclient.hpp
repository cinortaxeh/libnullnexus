/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <boost/asio/connect.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <queue>

namespace beast     = boost::beast;         // from <boost/beast.hpp>
namespace http      = beast::http;          // from <boost/beast/http.hpp>
namespace websocket = beast::websocket;     // from <boost/beast/websocket.hpp>
namespace net       = boost::asio;          // from <boost/asio.hpp>
using tcp           = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

class WebSocketClient
{
    // Settings
    std::string host, port, endpoint;
    // Message callback
    std::function<void(std::string)> callback;

    // ASIO
    std::recursive_mutex mutex;
    net::io_context ioc;
    std::optional<websocket::stream<tcp::socket>> ws;
    std::optional<net::executor_work_guard<decltype(ioc.get_executor())>> work;
    beast::flat_buffer buf;

    // Delayed start (after failed connect)
    net::deadline_timer start_delay_timer = net::deadline_timer(ioc);

    // Message list with higher chance of delivery
    net::deadline_timer message_queue_timer = net::deadline_timer(ioc);
    std::queue<std::string> messages;

    // Worker thread
    std::optional<std::thread> worker;

    // Internal variables
    bool shouldBeActive = false;

    void log(std::string msg)
    {
        std::cout << msg << std::endl;
    }

    // Reconnect after an error, run in a different thread than the worker
    void doReconnect()
    {
        std::lock_guard lock(mutex);
        if (!shouldBeActive)
            return;
        internalStop();
        internalStart();
    }

    void handle_handler_error(const boost::system::error_code &ec)
    {
        if (ec == net::error::basic_errors::operation_aborted)
            return;
        log(ec.message() + " " + std::to_string(ec.value()));
        std::thread t(&WebSocketClient::doReconnect, this);
        t.detach();
    }

    // Function to run the internal ASIO loop
    void runIO()
    {
        ioc.run();
        log("IOC exited");
    }

    // Function gets called whenever a message or error is sent
    void handler_onread(const boost::system::error_code &ec, std::size_t)
    {
        if (ec)
        {
            // Let someone else handle this error
            handle_handler_error(ec);
            return;
        }
        // Send message to callback
        callback(beast::buffers_to_string(buf.data()));
        buf.clear();
        // we technically stop reading after this call. We need to restart the handler.
        startAsyncRead();
    }

    // Start async reading from ASIO websocket
    void startAsyncRead()
    {
        ws->async_read(buf, beast::bind_front_handler(&WebSocketClient::handler_onread, this));
    }

    // Called by internalStart to run the actual connection code and return true/false
    bool doConnectionAttempt()
    {
        try
        {
            tcp::resolver resolver{ ioc };

            // Look up the domain name
            auto const results = resolver.resolve(host, port);

            // Create a new websocket, old one can't be used anymore after a .close() call
            ws.emplace(ioc);

            // Connect to the websocket
            net::connect(ws->next_layer(), results.begin(), results.end());
            // Set a decorator to change the User-Agent of the handshake
            ws->set_option(websocket::stream_base::decorator([](websocket::request_type &req) { req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro"); }));
            // Perform the websocket handshake
            ws->handshake(host, endpoint);

            log("CO: Connected to the server.");

            startAsyncRead();
            // Send cached messages
            trySendMessageQueue();
            return true;
        }
        catch (...)
        {
            // Some error. Trying again later.
            log("CO: Connection to server failed!");
            return false;
        }
    }

    // Use the io_context+worker to sheudule a restart
    void scheduleDelayedStart()
    {
        start_delay_timer.cancel();
        start_delay_timer.expires_from_now(boost::posix_time::seconds(10));
        start_delay_timer.async_wait(std::bind(&WebSocketClient::handler_startDelayTimer, this, std::placeholders::_1));
    }

    // React to the timer being activated
    void handler_startDelayTimer(const boost::system::error_code &ec)
    {
        if (ec)
            return;
        std::lock_guard lock(mutex);
        if (!shouldBeActive)
            return;
        if (!doConnectionAttempt())
            scheduleDelayedStart();
    }

    // Do everything needed to start
    void internalStart()
    {
        log("CO: Connecting to server");

        if (!worker)
        {
            // Create generic work object, runIO will never exit until this work is destructed
            work.emplace(ioc.get_executor());
            worker.emplace(&WebSocketClient::runIO, this);
        }

        if (!doConnectionAttempt())
            scheduleDelayedStart();
    }

    // Do everything needed to stop the socket
    void internalStop()
    {
        // Prevent more connect attempts
        start_delay_timer.cancel();
        if (ws && ws->is_open())
            ws->async_close(websocket::close_code::normal, [](const boost::system::error_code &) {});
        // Stop message queue from running while stopped
        message_queue_timer.cancel();

        if (worker)
        {
            // Allow runIO to exit
            work.reset();
            // Join and delete the thread
            worker->join();
            worker.reset();
            // Make IOC ready for the next run
            ioc.restart();
        }
    }

    /* Functions for handling the sending of messages */
    void handle_timerMessageQueue(const boost::system::error_code &ec)
    {
        if (ec)
            return;
        trySendMessageQueue();
    }

    void trySendMessageQueue()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!shouldBeActive || !ws || !ws->is_open())
            return;

        while (messages.size())
        {
            try
            {
                ws->write(net::buffer(messages.front()));
                messages.pop();
            }
            catch (...)
            {
                message_queue_timer.cancel();
                message_queue_timer.expires_from_now(boost::posix_time::seconds(1));
                message_queue_timer.async_wait(std::bind(&WebSocketClient::handle_timerMessageQueue, this, std::placeholders::_1));
                return;
            }
        }
    }
    /* ~Functions for handling the sending of messages~ */

public:
    void start()
    {
        std::lock_guard lock(mutex);
        if (shouldBeActive)
            return;
        shouldBeActive = true;
        internalStart();
    }
    void stop()
    {
        std::lock_guard lock(mutex);
        if (!shouldBeActive)
            return;
        shouldBeActive = false;
        internalStop();
        log("CO: Stopped!");
    }

    bool sendMessage(std::string msg, bool sendIfOffline = false)
    {
        std::lock_guard lock(mutex);
        if (sendIfOffline)
        {
            // Push into a queue
            messages.push(msg);
            // Try to send said queue
            trySendMessageQueue();
        }
        else
        {
            try
            {
                if (!ws)
                    return false;
                ws->write(net::buffer(msg));
            }
            catch (...)
            {
                return false;
            }
        }
        return true;
    }

    WebSocketClient(std::string host, std::string port, std::string endpoint, std::function<void(std::string)> callback) : host(host), port(port), endpoint(endpoint), callback(callback)
    {
    }

    ~WebSocketClient()
    {
        stop();
    }
};