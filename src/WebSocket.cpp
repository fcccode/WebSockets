/**
 * @file WebSocket.cpp
 *
 * This module contains the implementation of the WebSockets::WebSocket class.
 *
 * © 2018 by Richard Walters
 */

#include <Base64/Base64.hpp>
#include <functional>
#include <mutex>
#include <Hash/Sha1.hpp>
#include <Hash/Templates.hpp>
#include <queue>
#include <stdint.h>
#include <SystemAbstractions/CryptoRandom.hpp>
#include <SystemAbstractions/DiagnosticsSender.hpp>
#include <SystemAbstractions/StringExtensions.hpp>
#include <Utf8/Utf8.hpp>
#include <vector>
#include <WebSockets/WebSocket.hpp>

namespace {

    /**
     * This is the version of the WebSocket protocol that this class supports.
     */
    const std::string CURRENTLY_SUPPORTED_WEBSOCKET_VERSION = "13";

    /**
     * This is the required length of the Base64 decoding of the
     * "Sec-WebSocket-Key" header in HTTP requests that initiate a WebSocket
     * opening handshake.
     */
    constexpr size_t REQUIRED_WEBSOCKET_KEY_LENGTH = 16;

    /**
     * This is the string added to the "Sec-WebSocket-Key" before computing
     * the SHA-1 hash and Base64 encoding the result to form the
     * corresponding "Sec-WebSocket-Accept" value.
     */
    const std::string WEBSOCKET_KEY_SALT = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    /**
     * This is the bit to set in the first octet of a WebSocket frame
     * to indicate that the frame is the final one in a message.
     */
    constexpr uint8_t FIN = 0x80;

    /**
     * This is the bit to set in the second octet of a WebSocket frame
     * to indicate that the payload of the frame is masked, and that
     * a masking key is included.
     */
    constexpr uint8_t MASK = 0x80;

    /**
     * This is the opcode for a continuation frame.
     */
    constexpr uint8_t OPCODE_CONTINUATION = 0x00;

    /**
     * This is the opcode for a text frame.
     */
    constexpr uint8_t OPCODE_TEXT = 0x01;

    /**
     * This is the opcode for a binary frame.
     */
    constexpr uint8_t OPCODE_BINARY = 0x02;

    /**
     * This is the opcode for a close frame.
     */
    constexpr uint8_t OPCODE_CLOSE = 0x08;

    /**
     * This is the opcode for a ping frame.
     */
    constexpr uint8_t OPCODE_PING = 0x09;

    /**
     * This is the opcode for a pong frame.
     */
    constexpr uint8_t OPCODE_PONG = 0x0A;

    /**
     * This is the maximum length of a control frame payload.
     */
    constexpr size_t MAX_CONTROL_FRAME_DATA_LENGTH = 125;

    /**
     * This is used to track what kind of message is being
     * sent or received in fragments.
     */
    enum class FragmentedMessageType {
        /**
         * This means we're not sending/receiving any message.
         */
        None,

        /**
         * This means we're sending/receiving a text message.
         */
        Text,

        /**
         * This means we're sending/receiving a binary message.
         */
        Binary,
    };

    /**
     * This represents something that happens with a WebSocket that should be
     * reported through a user delegate.
     */
    struct Event {
        /**
         * This indicates the type of event that happened.
         */
        enum class Type {
            /**
             * This is the default type, used when an event isn't yet set up.
             */
            Unknown,

            /**
             * This indicates a text message was received.
             */
            Text,

            /**
             * This indicates a binary message was received.
             */
            Binary,

            /**
             * This indicates a ping was received.
             */
            Ping,

            /**
             * This indicates a pong was received.
             */
            Pong,

            /**
             * This indicates the WebSocket was closed.
             */
            Close,
        } type = Type::Unknown;

        /**
         * This is the data payload from the message.
         */
        std::string content;

        /**
         * If the event is a close, this is the status code from the received
         * close frame.
         */
        unsigned int closeCode;
    };

    /**
     * This function computes the value of the "Sec-WebSocket-Accept"
     * HTTP response header that matches the given value of the
     * "Sec-WebSocket-Key" HTTP request header.
     *
     * @param[in] key
     *     This is the key value for which to compute the matching answer.
     *
     * @return
     *     The answer computed from the given key is returned.
     */
    std::string ComputeKeyAnswer(const std::string& key) {
        return Base64::Encode(
            Hash::StringToBytes< Hash::Sha1 >(key + WEBSOCKET_KEY_SALT)
        );
    }

}

namespace WebSockets {

    /**
     * This contains the private properties of a WebSocket instance.
     */
    struct WebSocket::Impl {
        // Properties

        /**
         * This holds onto variables which the user can change that modifies
         * the behavior of the WebSocket.
         */
        Configuration configuration;

        /**
         * This is a helper object used to generate and publish
         * diagnostic messages.
         */
        SystemAbstractions::DiagnosticsSender diagnosticsSender;

        /**
         * This is used to synchronize access to the WebSocket.
         */
        std::recursive_mutex mutex;

        /**
         * This is the queue of events waiting to be reported through
         * delegates.  They sit here until a delegate is registered and the
         * WebSocket's mutex is not being held (to prevent deadlocks).
         */
        std::queue< Event > eventQueue;

        /**
         * This is the connection to use to send and receive frames.
         */
        std::shared_ptr< Http::Connection > connection;

        /**
         * This is the role to play in the connection.
         */
        Role role;

        /**
         * This is the Base64 encoded randomly-generated data set for
         * the Sec-WebSocket-Key header sent in the HTTP request
         * of the opening handshake, when opening as a client.
         */
        std::string key;

        /**
         * This flag indicates whether or not the WebSocket has sent
         * a close frame, and is waiting for a one to be received
         * back before closing the WebSocket.
         */
        bool closeSent = false;

        /**
         * This flag indicates whether or not the WebSocket has received
         * a close frame, and is waiting for the user to finish up
         * and signal a close, in order to close the WebSocket.
         */
        bool closeReceived = false;

        /**
         * This indicates what type of message the WebSocket is in the midst
         * of sending, if any.
         */
        FragmentedMessageType sending = FragmentedMessageType::None;

        /**
         * This indicates what type of message the WebSocket is in the midst
         * of receiving, if any.
         */
        FragmentedMessageType receiving = FragmentedMessageType::None;

        /**
         * This holds the functions to call whenever anything interesting
         * happens.
         */
        Delegates delegates;

        /**
         * This indicates whether or not delegates have been set for the
         * WebSocket.  Until this is true, the event queue is not emptied, so
         * that any events will not be lost.
         */
        bool delegatesSet = false;

        /**
         * This is where we put data received before it's been
         * reassembled into frames.
         */
        std::vector< uint8_t > frameReassemblyBuffer;

        /**
         * This is where we put frames received before they've been
         * reassembled into messages.
         */
        std::string messageReassemblyBuffer;

        /**
         * This is used to generate masking keys that have strong entropy.
         */
        SystemAbstractions::CryptoRandom rng;

        // Methods

        /**
         * This is the constructor for the structure.
         */
        Impl()
            : diagnosticsSender("WebSockets::WebSocket")
        {
        }

        /**
         * This method safely processes the event queue.  For each event,
         * if a corresponding delegate is registered, the delegate is
         * called and the event is removed from the queue.
         */
        void ProcessEventQueue() {
            std::unique_lock< decltype(mutex) > lock(mutex);
            if (!delegatesSet) {
                return;
            }
            auto delegatesCopy = delegates;
            std::queue< Event > offloadedEvents;
            offloadedEvents.swap(eventQueue);
            lock.unlock();
            while (!offloadedEvents.empty()) {
                auto& event = offloadedEvents.front();
                switch (event.type) {
                    case Event::Type::Text: {
                        if (delegatesCopy.text != nullptr) {
                            delegatesCopy.text(std::move(event.content));
                        }
                    } break;

                    case Event::Type::Binary: {
                        if (delegatesCopy.binary != nullptr) {
                            delegatesCopy.binary(std::move(event.content));
                        }
                    } break;

                    case Event::Type::Ping: {
                        if (delegatesCopy.ping != nullptr) {
                            delegatesCopy.ping(std::move(event.content));
                        }
                    } break;

                    case Event::Type::Pong: {
                        if (delegatesCopy.pong != nullptr) {
                            delegatesCopy.pong(std::move(event.content));
                        }
                    } break;

                    case Event::Type::Close: {
                        if (delegatesCopy.close != nullptr) {
                            delegatesCopy.close(
                                event.closeCode,
                                std::move(event.content)
                            );
                        }
                    } break;

                    default: break;
                }
                offloadedEvents.pop();
            }
        }

        /**
         * This method responds to the WebSocket being closed.
         *
         * @param[in] code
         *     This is the status code of the closure.
         *
         * @param[in] reason
         *     This is the reason text of the closure.
         */
        void OnClose(
            unsigned int code,
            const std::string& reason
        ) {
            const auto closeSentEarlier = closeSent;
            closeReceived = true;
            Event event;
            event.type = Event::Type::Close;
            event.closeCode = code;
            event.content = reason;
            eventQueue.push(std::move(event));
            if (closeSentEarlier) {
                connection->Break(false);
            }
        }

        /**
         * This method is called if a text message has been received.
         *
         * @param[in] message
         *     This is the text message that has been received.
         */
        void OnTextMessage(std::string&& message) {
            Utf8::Utf8 utf8;
            if (utf8.IsValidEncoding(message)) {
                Event event;
                event.type = Event::Type::Text;
                event.content = std::move(message);
                eventQueue.push(std::move(event));
            } else {
                Close(1007, "invalid UTF-8 encoding in text message", true);
            }
        }

        /**
         * This method is called if a binary message has been received.
         *
         * @param[in] message
         *     This is the binary message that has been received.
         */
        void OnBinaryMessage(std::string&& message) {
            Event event;
            event.type = Event::Type::Binary;
            event.content = std::move(message);
            eventQueue.push(std::move(event));
        }

        /**
         * This method initiates the closing of the WebSocket,
         * sending a close frame with the given status code and reason.
         *
         * @param[in] code
         *     This is the status code to send in the close frame.
         *
         * @param[in] reason
         *     This is the reason text to send in the close frame.
         *
         * @param[in] fail
         *     This indicates whether or not to fail the connection,
         *     closing the connection and reporting the close
         *     immediately, rather than waiting for the receipt
         *     of a close frame from the remote peer.
         */
        void Close(
            unsigned int code,
            const std::string reason,
            bool fail = false
        ) {
            if (closeSent) {
                return;
            }
            closeSent = true;
            if (code == 1006) {
                OnClose(code, reason);
            } else {
                std::string data;
                if (code != 1005) {
                    data.push_back((uint8_t)(code >> 8));
                    data.push_back((uint8_t)(code & 0xFF));
                    data += reason;
                }
                SendFrame(true, OPCODE_CLOSE, data);
                if (fail) {
                    OnClose(code, reason);
                } else if (closeReceived) {
                    connection->Break(true);
                }
                diagnosticsSender.SendDiagnosticInformationFormatted(
                    1,
                    "Connection to %s closed (%s)",
                    connection->GetPeerId().c_str(),
                    reason.c_str()
                );
            }
        }

        /**
         * This method constructs and sends a frame from the WebSocket.
         *
         * @param[in] fin
         *     This indicates whether or not to set the FIN bit in the frame.
         *
         * @param[in] opcode
         *     This is the opcode to set in the frame.
         *
         * @param[in] payload
         *     This is the payload to include in the frame.
         */
        void SendFrame(
            bool fin,
            uint8_t opcode,
            const std::string& payload
        ) {
            std::vector< uint8_t > frame;
            frame.push_back(
                (fin ? FIN : 0)
                + opcode
            );
            const uint8_t mask = ((role == Role::Client) ? MASK : 0);
            if (payload.length() < 126) {
                frame.push_back((uint8_t)payload.length() + mask);
            } else if (payload.length() < 65536) {
                frame.push_back(0x7E + mask);
                frame.push_back((uint8_t)(payload.length() >> 8));
                frame.push_back((uint8_t)(payload.length() & 0xFF));
            } else {
                frame.push_back(0x7F + mask);
                frame.push_back((uint8_t)(payload.length() >> 56));
                frame.push_back((uint8_t)((payload.length() >> 48) & 0xFF));
                frame.push_back((uint8_t)((payload.length() >> 40) & 0xFF));
                frame.push_back((uint8_t)((payload.length() >> 32) & 0xFF));
                frame.push_back((uint8_t)((payload.length() >> 24) & 0xFF));
                frame.push_back((uint8_t)((payload.length() >> 16) & 0xFF));
                frame.push_back((uint8_t)((payload.length() >> 8) & 0xFF));
                frame.push_back((uint8_t)(payload.length() & 0xFF));
            }
            if (mask == 0) {
                (void)frame.insert(
                    frame.end(),
                    payload.begin(),
                    payload.end()
                );
            } else {
                uint8_t maskingKey[4];
                rng.Generate(maskingKey, sizeof(maskingKey));
                for (size_t i = 0; i < sizeof(maskingKey); ++i) {
                    frame.push_back(maskingKey[i]);
                }
                for (size_t i = 0; i < payload.length(); ++i) {
                    frame.push_back(payload[i] ^ maskingKey[i % 4]);
                }
            }
            connection->SendData(frame);
        }

        /**
         * This method is called whenever the WebSocket has reassembled
         * a complete frame received from the remote peer.
         *
         * @param[in] headerLength
         *     This is the size of the frame header, in octets.
         *
         * @param[in] payloadLength
         *     This is the size of the frame payload, in octets.
         */
        void ReceiveFrame(
            size_t headerLength,
            size_t payloadLength
        ) {
            if (closeReceived) {
                return;
            }
            const bool fin = ((frameReassemblyBuffer[0] & FIN) != 0);
            const uint8_t reservedBits = ((frameReassemblyBuffer[0] >> 4) & 0x07);
            if (reservedBits != 0) {
                Close(1002, "reserved bits set", true);
                return;
            }
            const bool mask = ((frameReassemblyBuffer[1] & MASK) != 0);
            if (mask) {
                if (role == Role::Client) {
                    Close(1002, "masked frame", true);
                    return;
                }
            } else {
                if (role == Role::Server) {
                    Close(1002, "unmasked frame", true);
                    return;
                }
            }
            const uint8_t opcode = (frameReassemblyBuffer[0] & 0x0F);
            std::string data;
            if (role == Role::Server) {
                data.resize(payloadLength);
                for (size_t i = 0; i < payloadLength; ++i) {
                    data[i] = (
                        frameReassemblyBuffer[headerLength + i]
                        ^ frameReassemblyBuffer[headerLength - 4 + (i % 4)]
                    );
                }
            } else {
                (void)data.assign(
                    frameReassemblyBuffer.begin() + headerLength,
                    frameReassemblyBuffer.begin() + headerLength + payloadLength
                );
            }
            switch (opcode) {
                case OPCODE_CONTINUATION: {
                    messageReassemblyBuffer += data;
                    switch (receiving) {
                        case FragmentedMessageType::Text: {
                            if (fin) {
                                OnTextMessage(std::move(messageReassemblyBuffer));
                            }
                        } break;

                        case FragmentedMessageType::Binary: {
                            if (fin) {
                                OnBinaryMessage(std::move(messageReassemblyBuffer));
                            }
                        } break;

                        default: {
                            messageReassemblyBuffer.clear();
                            Close(1002, "unexpected continuation frame", true);
                        } break;
                    }
                    if (fin) {
                        receiving = FragmentedMessageType::None;
                        messageReassemblyBuffer.clear();
                    }
                } break;

                case OPCODE_TEXT: {
                    if (receiving == FragmentedMessageType::None) {
                        if (fin) {
                            OnTextMessage(std::move(data));
                        } else {
                            receiving = FragmentedMessageType::Text;
                            messageReassemblyBuffer = data;
                        }
                    } else {
                        Close(1002, "last message incomplete", true);
                    }
                } break;

                case OPCODE_BINARY: {
                    if (receiving == FragmentedMessageType::None) {
                        if (fin) {
                            OnBinaryMessage(std::move(data));
                        } else {
                            receiving = FragmentedMessageType::Binary;
                            messageReassemblyBuffer = data;
                        }
                    } else {
                        Close(1002, "last message incomplete", true);
                    }
                } break;

                case OPCODE_CLOSE: {
                    unsigned int code = 1005;
                    std::string reason;
                    bool fail = false;
                    if (data.length() >= 2) {
                        code = (
                            (((unsigned int)data[0] << 8) & 0xFF00)
                            + ((unsigned int)data[1] & 0x00FF)
                        );
                        reason = data.substr(2);
                        Utf8::Utf8 utf8;
                        if (!utf8.IsValidEncoding(reason)) {
                            Close(1007, "invalid UTF-8 encoding in close reason", true);
                            fail = true;
                        }
                    }
                    if (!fail) {
                        OnClose(code, reason);
                        diagnosticsSender.SendDiagnosticInformationFormatted(
                            1,
                            "Connection to %s closed by peer",
                            connection->GetPeerId().c_str()
                        );
                    }
                } break;

                case OPCODE_PING: {
                    SendFrame(true, OPCODE_PONG, data);
                    Event event;
                    event.type = Event::Type::Ping;
                    event.content = std::move(data);
                    eventQueue.push(std::move(event));
                } break;

                case OPCODE_PONG: {
                    Event event;
                    event.type = Event::Type::Pong;
                    event.content = std::move(data);
                    eventQueue.push(std::move(event));
                } break;

                default: {
                    Close(1002, "unknown opcode", true);
                } break;
            }
        }

        /**
         * This method is called whenever the WebSocket receives data from
         * the remote peer.
         *
         * @param[in] data
         *     This is the data received from the remote peer.
         */
        void ReceiveData(
            const std::vector< uint8_t >& data
        ) {
            std::lock_guard< decltype(mutex) > lock(mutex);
            if (connection == nullptr) {
                return;
            }
            if (
                (configuration.maxFrameSize > 0)
                && (
                    frameReassemblyBuffer.size() + data.size()
                    > configuration.maxFrameSize
                )
            ) {
                Close(1009, "frame too large", true);
                return;
            }
            (void)frameReassemblyBuffer.insert(
                frameReassemblyBuffer.end(),
                data.begin(),
                data.end()
            );
            for(;;) {
                if (frameReassemblyBuffer.size() < 2) {
                    return;
                }
                const auto lengthFirstOctet = (frameReassemblyBuffer[1] & ~MASK);
                size_t headerLength, payloadLength;
                if (lengthFirstOctet == 0x7E) {
                    headerLength = 4;
                    if (frameReassemblyBuffer.size() < headerLength) {
                        return;
                    }
                    payloadLength = (
                        ((size_t)frameReassemblyBuffer[2] << 8)
                        + (size_t)frameReassemblyBuffer[3]
                    );
                } else if (lengthFirstOctet == 0x7F) {
                    headerLength = 10;
                    if (frameReassemblyBuffer.size() < headerLength) {
                        return;
                    }
                    payloadLength = (
                        ((size_t)frameReassemblyBuffer[2] << 56)
                        + ((size_t)frameReassemblyBuffer[3] << 48)
                        + ((size_t)frameReassemblyBuffer[4] << 40)
                        + ((size_t)frameReassemblyBuffer[5] << 32)
                        + ((size_t)frameReassemblyBuffer[6] << 24)
                        + ((size_t)frameReassemblyBuffer[7] << 16)
                        + ((size_t)frameReassemblyBuffer[8] << 8)
                        + (size_t)frameReassemblyBuffer[9]
                    );
                } else {
                    headerLength = 2;
                    payloadLength = (size_t)lengthFirstOctet;
                }
                if ((frameReassemblyBuffer[1] & MASK) != 0) {
                    headerLength += 4;
                }
                if (frameReassemblyBuffer.size() < headerLength + payloadLength) {
                    return;
                }
                ReceiveFrame(headerLength, payloadLength);
                (void)frameReassemblyBuffer.erase(
                    frameReassemblyBuffer.begin(),
                    frameReassemblyBuffer.begin() + headerLength + payloadLength
                );
            }
        }

        /**
         * This method is called if the connection is broken by
         * the remote peer.
         */
        void ConnectionBroken() {
            std::lock_guard< decltype(mutex) > lock(mutex);
            if (connection == nullptr) {
                return;
            }
            Close(1006, "connection broken by peer", true);
            diagnosticsSender.SendDiagnosticInformationFormatted(
                1,
                "Connection to %s broken by peer",
                connection->GetPeerId().c_str()
            );
        }
    };

    WebSocket::~WebSocket() noexcept = default;
    WebSocket::WebSocket(WebSocket&&) noexcept = default;
    WebSocket& WebSocket::operator=(WebSocket&&) noexcept = default;

    WebSocket::WebSocket()
        : impl_(new Impl)
    {
    }

    SystemAbstractions::DiagnosticsSender::UnsubscribeDelegate WebSocket::SubscribeToDiagnostics(
        SystemAbstractions::DiagnosticsSender::DiagnosticMessageDelegate delegate,
        size_t minLevel
    ) {
        return impl_->diagnosticsSender.SubscribeToDiagnostics(delegate, minLevel);
    }

    void WebSocket::Configure(Configuration configuration) {
        std::lock_guard< decltype(impl_->mutex) > lock(impl_->mutex);
        impl_->configuration = configuration;
    }

    void WebSocket::StartOpenAsClient(
        Http::Request& request
    ) {
        request.headers.SetHeader("Sec-WebSocket-Version", CURRENTLY_SUPPORTED_WEBSOCKET_VERSION);
        char nonce[16];
        impl_->rng.Generate(nonce, sizeof(nonce));
        impl_->key = Base64::Encode(
            std::string(nonce, sizeof(nonce))
        );
        request.headers.SetHeader("Sec-WebSocket-Key", impl_->key);
        request.headers.SetHeader("Upgrade", "websocket");
        auto connectionTokens = request.headers.GetHeaderTokens("Connection");
        connectionTokens.push_back("upgrade");
        request.headers.SetHeader("Connection", connectionTokens, true);
    }

    bool WebSocket::FinishOpenAsClient(
        std::shared_ptr< Http::Connection > connection,
        const Http::Response& response
    ) {
        if (response.statusCode != 101) {
            return false;
        }
        if (!response.headers.HasHeaderToken("Connection", "upgrade")) {
            return false;
        }
        if (SystemAbstractions::ToLower(response.headers.GetHeaderValue("Upgrade")) != "websocket") {
            return false;
        }
        if (response.headers.GetHeaderValue("Sec-WebSocket-Accept") != ComputeKeyAnswer(impl_->key)) {
            return false;
        }
        if (!response.headers.GetHeaderTokens("Sec-WebSocket-Extensions").empty()) {
            return false;
        }
        if (!response.headers.GetHeaderTokens("Sec-WebSocket-Protocol").empty()) {
            return false;
        }
        Open(connection, Role::Client);
        return true;
    }

    bool WebSocket::OpenAsServer(
        std::shared_ptr< Http::Connection > connection,
        const Http::Request& request,
        Http::Response& response,
        const std::string& trailer
    ) {
        if (request.method != "GET") {
            return false;
        }
        if (!request.headers.HasHeaderToken("Connection", "upgrade")) {
            return false;
        }
        if (SystemAbstractions::ToLower(request.headers.GetHeaderValue("Upgrade")) != "websocket") {
            return false;
        }
        if (request.headers.GetHeaderValue("Sec-WebSocket-Version") != CURRENTLY_SUPPORTED_WEBSOCKET_VERSION) {
            response.statusCode = 400;
            response.reasonPhrase = "Bad Request";
            return false;
        }
        if (!trailer.empty()) {
            response.statusCode = 400;
            response.reasonPhrase = "Bad Request";
            return false;
        }
        impl_->key = request.headers.GetHeaderValue("Sec-WebSocket-Key");
        if (Base64::Decode(impl_->key).length() != REQUIRED_WEBSOCKET_KEY_LENGTH) {
            response.statusCode = 400;
            response.reasonPhrase = "Bad Request";
            return false;
        }
        auto connectionTokens = response.headers.GetHeaderTokens("Connection");
        connectionTokens.push_back("upgrade");
        response.statusCode = 101;
        response.reasonPhrase = "Switching Protocols";
        response.headers.SetHeader("Connection", connectionTokens, true);
        response.headers.SetHeader("Upgrade", "websocket");
        response.headers.SetHeader("Sec-WebSocket-Accept", ComputeKeyAnswer(impl_->key));
        Open(connection, Role::Server);
        return true;
    }

    void WebSocket::Open(
        std::shared_ptr< Http::Connection > connection,
        Role role
    ) {
        impl_->connection = connection;
        impl_->role = role;
        std::weak_ptr< Impl > implWeak(impl_);
        impl_->connection->SetDataReceivedDelegate(
            [implWeak](
                const std::vector< uint8_t >& data
            ){
                const auto impl = implWeak.lock();
                if (impl) {
                    impl->ReceiveData(data);
                    impl->ProcessEventQueue();
                }
            }
        );
        impl_->connection->SetBrokenDelegate(
            [implWeak](bool){
                const auto impl = implWeak.lock();
                if (impl) {
                    impl->ConnectionBroken();
                    impl->ProcessEventQueue();
                }
            }
        );
    }

    void WebSocket::Close(
        unsigned int code,
        const std::string reason
    ) {
        std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
        if (impl_->connection == nullptr) {
            return;
        }
        impl_->Close(code, reason);
        lock.unlock();
        impl_->ProcessEventQueue();
    }

    void WebSocket::Ping(const std::string& data) {
        std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
        if (impl_->connection == nullptr) {
            return;
        }
        if (impl_->closeSent) {
            return;
        }
        if (data.length() > MAX_CONTROL_FRAME_DATA_LENGTH) {
            return;
        }
        impl_->SendFrame(true, OPCODE_PING, data);
        lock.unlock();
        impl_->ProcessEventQueue();
    }

    void WebSocket::Pong(const std::string& data) {
        std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
        if (impl_->connection == nullptr) {
            return;
        }
        if (impl_->closeSent) {
            return;
        }
        if (data.length() > MAX_CONTROL_FRAME_DATA_LENGTH) {
            return;
        }
        impl_->SendFrame(true, OPCODE_PONG, data);
        lock.unlock();
        impl_->ProcessEventQueue();
    }

    void WebSocket::SendText(
        const std::string& data,
        bool lastFragment
    ) {
        std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
        if (impl_->connection == nullptr) {
            return;
        }
        if (impl_->closeSent) {
            return;
        }
        if (impl_->sending == FragmentedMessageType::Binary) {
            return;
        }
        const auto opcode = (
            (impl_->sending == FragmentedMessageType::Text)
            ? OPCODE_CONTINUATION
            : OPCODE_TEXT
        );
        impl_->SendFrame(lastFragment, opcode, data);
        impl_->sending = (
            lastFragment
            ? FragmentedMessageType::None
            : FragmentedMessageType::Text
        );
        lock.unlock();
        impl_->ProcessEventQueue();
    }

    void WebSocket::SendBinary(
        const std::string& data,
        bool lastFragment
    ) {
        std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
        if (impl_->connection == nullptr) {
            return;
        }
        if (impl_->closeSent) {
            return;
        }
        if (impl_->sending == FragmentedMessageType::Text) {
            return;
        }
        const auto opcode = (
            (impl_->sending == FragmentedMessageType::Binary)
            ? OPCODE_CONTINUATION
            : OPCODE_BINARY
        );
        impl_->SendFrame(lastFragment, opcode, data);
        impl_->sending = (
            lastFragment
            ? FragmentedMessageType::None
            : FragmentedMessageType::Binary
        );
        lock.unlock();
        impl_->ProcessEventQueue();
    }

    void WebSocket::SetDelegates(Delegates&& delegates) {
        std::unique_lock< decltype(impl_->mutex) > lock(impl_->mutex);
        impl_->delegates = std::move(delegates);
        impl_->delegatesSet = true;
        lock.unlock();
        impl_->ProcessEventQueue();
    }

}
