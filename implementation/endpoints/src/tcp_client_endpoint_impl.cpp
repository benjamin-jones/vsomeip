// Copyright (C) 2014-2016 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <iomanip>

#include <boost/asio/write.hpp>

#include <vsomeip/constants.hpp>
#include <vsomeip/defines.hpp>

#include "../include/endpoint_host.hpp"
#include "../include/tcp_client_endpoint_impl.hpp"
#include "../../logging/include/logger.hpp"
#include "../../utility/include/utility.hpp"
#include "../../configuration/include/internal.hpp"

namespace ip = boost::asio::ip;

namespace vsomeip {

tcp_client_endpoint_impl::tcp_client_endpoint_impl(
        std::shared_ptr< endpoint_host > _host,
        endpoint_type _local,
        endpoint_type _remote,
        boost::asio::io_service &_io,
        std::uint32_t _max_message_size,
        std::uint32_t _buffer_shrink_threshold)
    : tcp_client_endpoint_base_impl(_host, _local, _remote, _io, _max_message_size),
      recv_buffer_size_initial_(VSOMEIP_SOMEIP_HEADER_SIZE),
      recv_buffer_(recv_buffer_size_initial_, 0),
      recv_buffer_size_(0),
      missing_capacity_(0),
      shrink_count_(0),
      buffer_shrink_threshold_(_buffer_shrink_threshold) {
    is_supporting_magic_cookies_ = true;
}

tcp_client_endpoint_impl::~tcp_client_endpoint_impl() {
    std::shared_ptr<endpoint_host> its_host = host_.lock();
    if (its_host) {
        its_host->release_port(local_.port(), true);
    }
}

bool tcp_client_endpoint_impl::is_local() const {
    return false;
}

void tcp_client_endpoint_impl::start() {
    connect();
}

void tcp_client_endpoint_impl::connect() {
    boost::system::error_code its_error;
    socket_.open(remote_.protocol(), its_error);

    if (!its_error || its_error == boost::asio::error::already_open) {
        // Nagle algorithm off
        socket_.set_option(ip::tcp::no_delay(true), its_error);

        // Enable SO_REUSEADDR to avoid bind problems with services going offline
        // and coming online again and the user has specified only a small number
        // of ports in the clients section for one service instance
        socket_.set_option(boost::asio::socket_base::reuse_address(true), its_error);

        // In case a client endpoint port was configured,
        // bind to it before connecting
        if (local_.port() != ILLEGAL_PORT) {
            boost::system::error_code its_bind_error;
            socket_.bind(local_, its_bind_error);
            if(its_bind_error) {
                VSOMEIP_WARNING << "tcp_client_endpoint::connect: "
                        "Error binding socket: " << its_bind_error.message();
            }
        }

        socket_.async_connect(
            remote_,
            std::bind(
                &tcp_client_endpoint_base_impl::connect_cbk,
                shared_from_this(),
                std::placeholders::_1
            )
        );
    } else {
        VSOMEIP_WARNING << "tcp_client_endpoint::connect: Error opening socket: "
                << its_error.message();
    }
}

void tcp_client_endpoint_impl::receive() {
    if(socket_.is_open()) {
        const std::size_t its_capacity(recv_buffer_.capacity());
        size_t buffer_size = its_capacity - recv_buffer_size_;
        if (missing_capacity_) {
            const std::size_t its_required_capacity(recv_buffer_size_ + missing_capacity_);
            if (its_capacity < its_required_capacity) {
                recv_buffer_.reserve(its_required_capacity);
                recv_buffer_.resize(its_required_capacity, 0x0);
            }
            buffer_size = missing_capacity_;
            missing_capacity_ = 0;
        } else if (buffer_shrink_threshold_
                && shrink_count_ > buffer_shrink_threshold_
                && recv_buffer_size_ == 0) {
            recv_buffer_.resize(recv_buffer_size_initial_, 0x0);
            recv_buffer_.shrink_to_fit();
            buffer_size = recv_buffer_size_initial_;
            shrink_count_ = 0;
        }
        socket_.async_receive(
            boost::asio::buffer(&recv_buffer_[recv_buffer_size_], buffer_size),
            std::bind(
                &tcp_client_endpoint_impl::receive_cbk,
                std::dynamic_pointer_cast< tcp_client_endpoint_impl >(shared_from_this()),
                std::placeholders::_1,
                std::placeholders::_2
            )
        );
    }
}

void tcp_client_endpoint_impl::send_queued() {
    message_buffer_ptr_t its_buffer;
    if(queue_.size()) {
        its_buffer = queue_.front();
    } else {
        return;
    }

    if (has_enabled_magic_cookies_)
        send_magic_cookie(its_buffer);

#if 0
    std::stringstream msg;
    msg << "tcei<" << remote_.address() << ":"
        << std::dec << remote_.port()  << ">::sq: ";
    for (std::size_t i = 0; i < its_buffer->size(); i++)
        msg << std::hex << std::setw(2) << std::setfill('0')
            << (int)(*its_buffer)[i] << " ";
    VSOMEIP_INFO << msg.str();
#endif

    boost::asio::async_write(
        socket_,
        boost::asio::buffer(*its_buffer),
        std::bind(
            &tcp_client_endpoint_base_impl::send_cbk,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2
        )
    );
}

bool tcp_client_endpoint_impl::get_remote_address(
        boost::asio::ip::address &_address) const {
    const boost::asio::ip::address its_address = remote_.address();
    if (its_address.is_unspecified()) {
        return false;
    }
    _address = its_address;
    return true;
}

unsigned short tcp_client_endpoint_impl::get_local_port() const {
    boost::system::error_code its_error;
    if (socket_.is_open()) {
        return socket_.local_endpoint(its_error).port();
    }
    return 0;
}

unsigned short tcp_client_endpoint_impl::get_remote_port() const {
    boost::system::error_code its_error;
    if (socket_.is_open()) {
        return socket_.remote_endpoint(its_error).port();
    }
    return 0;
}

bool tcp_client_endpoint_impl::is_reliable() const {
  return true;
}

bool tcp_client_endpoint_impl::is_magic_cookie(size_t _offset) const {
    return (0 == std::memcmp(SERVICE_COOKIE, &recv_buffer_[_offset], sizeof(SERVICE_COOKIE)));
}

void tcp_client_endpoint_impl::send_magic_cookie(message_buffer_ptr_t &_buffer) {
    if (max_message_size_ == MESSAGE_SIZE_UNLIMITED
            || max_message_size_ - _buffer->size() >=
        VSOMEIP_SOMEIP_HEADER_SIZE + VSOMEIP_SOMEIP_MAGIC_COOKIE_SIZE) {
        _buffer->insert(
            _buffer->begin(),
            CLIENT_COOKIE,
            CLIENT_COOKIE + sizeof(CLIENT_COOKIE)
        );
    } else {
        VSOMEIP_WARNING << "Packet full. Cannot insert magic cookie!";
    }
}

void tcp_client_endpoint_impl::receive_cbk(
        boost::system::error_code const &_error, std::size_t _bytes) {
    if (_error == boost::asio::error::operation_aborted) {
        // endpoint was stopped
        shutdown_and_close_socket();
        return;
    }
#if 0
    std::stringstream msg;
    msg << "cei::rcb (" << _error.message() << "): ";
    for (std::size_t i = 0; i < _bytes + recv_buffer_size_; ++i)
        msg << std::hex << std::setw(2) << std::setfill('0')
            << (int) recv_buffer_[i] << " ";
    VSOMEIP_INFO << msg.str();
#endif
    std::shared_ptr<endpoint_host> its_host = host_.lock();
    if (its_host) {
        if (!_error && 0 < _bytes) {
            recv_buffer_size_ += _bytes;

            boost::system::error_code its_error;
            endpoint_type its_endpoint(socket_.remote_endpoint(its_error));
            const boost::asio::ip::address its_remote_address(its_endpoint.address());
            const std::uint16_t its_remote_port(its_endpoint.port());
            size_t its_iteration_gap = 0;
            bool has_full_message;
            do {
                uint32_t current_message_size
                    = utility::get_message_size(&recv_buffer_[its_iteration_gap],
                            (uint32_t) recv_buffer_size_);
                has_full_message = (current_message_size > 0
                                 && current_message_size <= recv_buffer_size_);
                if (has_full_message) {
                    bool needs_forwarding(true);
                    if (is_magic_cookie(its_iteration_gap)) {
                        has_enabled_magic_cookies_ = true;
                    } else {
                        if (has_enabled_magic_cookies_) {
                            uint32_t its_offset = find_magic_cookie(&recv_buffer_[its_iteration_gap],
                                    (uint32_t) recv_buffer_size_);
                            if (its_offset < current_message_size) {
                                VSOMEIP_ERROR << "Message includes Magic Cookie. Ignoring it.";
                                current_message_size = its_offset;
                                needs_forwarding = false;
                            }
                        }
                    }
                    if (needs_forwarding) {
                        if (!has_enabled_magic_cookies_) {
                            its_host->on_message(&recv_buffer_[its_iteration_gap],
                                                 current_message_size, this,
                                                 boost::asio::ip::address(),
                                                 VSOMEIP_ROUTING_CLIENT,
                                                 its_remote_address,
                                                 its_remote_port);
                        } else {
                            // Only call on_message without a magic cookie in front of the buffer!
                            if (!is_magic_cookie(its_iteration_gap)) {
                                its_host->on_message(&recv_buffer_[its_iteration_gap],
                                                     current_message_size, this,
                                                     boost::asio::ip::address(),
                                                     VSOMEIP_ROUTING_CLIENT,
                                                     its_remote_address,
                                                     its_remote_port);
                            }
                        }
                    }
                    calculate_shrink_count();
                    recv_buffer_size_ -= current_message_size;
                    its_iteration_gap += current_message_size;
                    missing_capacity_ = 0;
                } else if (current_message_size > recv_buffer_size_) {
                        missing_capacity_ = current_message_size
                                - static_cast<std::uint32_t>(recv_buffer_size_);
                } else if (VSOMEIP_SOMEIP_HEADER_SIZE > recv_buffer_size_) {
                        missing_capacity_ = VSOMEIP_SOMEIP_HEADER_SIZE
                                - static_cast<std::uint32_t>(recv_buffer_size_);
                } else if (has_enabled_magic_cookies_ && recv_buffer_size_ > 0) {
                    uint32_t its_offset = find_magic_cookie(&recv_buffer_[its_iteration_gap], recv_buffer_size_);
                    if (its_offset < recv_buffer_size_) {
                        recv_buffer_size_ -= its_offset;
                        its_iteration_gap += its_offset;
                        has_full_message = true; // trigger next loop
                    }
                } else if (max_message_size_ != MESSAGE_SIZE_UNLIMITED &&
                        current_message_size > max_message_size_) {
                    if (has_enabled_magic_cookies_) {
                        VSOMEIP_ERROR << "Received a TCP message which exceeds "
                                      << "maximum message size ("
                                      << std::dec << current_message_size
                                      << "). Magic Cookies are enabled: "
                                      << "Resetting receiver.";
                        recv_buffer_size_ = 0;
                    } else {
                        VSOMEIP_ERROR << "Received a TCP message which exceeds "
                                      << "maximum message size ("
                                      << std::dec << current_message_size
                                      << ") Magic cookies are disabled: "
                                      << "Client will be disabled!";
                        recv_buffer_size_ = 0;
                        return;
                    }
                } else {
                        VSOMEIP_ERROR << "tce::c<" << this
                                << ">rcb: recv_buffer_size is: " << std::dec
                                << recv_buffer_size_ << " but couldn't read "
                                "out message_size. recv_buffer_capacity: "
                                << recv_buffer_.capacity()
                                << " its_iteration_gap: " << its_iteration_gap;
                }
            } while (has_full_message && recv_buffer_size_);
            if (its_iteration_gap) {
                // Copy incomplete message to front for next receive_cbk iteration
                for (size_t i = 0; i < recv_buffer_size_; ++i) {
                    recv_buffer_[i] = recv_buffer_[i + its_iteration_gap];
                }
                // Still more capacity needed after shifting everything to front?
                if (missing_capacity_ &&
                        missing_capacity_ <= recv_buffer_.capacity() - recv_buffer_size_) {
                    missing_capacity_ = 0;
                }
            }
            receive();
        } else {
            if (_error == boost::asio::error::connection_reset ||
                    _error ==  boost::asio::error::eof) {
                VSOMEIP_TRACE << "tcp_client_endpoint: connection_reseted/EOF ~> close socket!";
                shutdown_and_close_socket();
            } else if (socket_.is_open()) {
                receive();
            }
        }
    }
}

void tcp_client_endpoint_impl::calculate_shrink_count() {
    if (buffer_shrink_threshold_) {
        if (recv_buffer_.capacity() != recv_buffer_size_initial_) {
            if (recv_buffer_size_ < (recv_buffer_.capacity() >> 1)) {
                shrink_count_++;
            } else {
                shrink_count_ = 0;
            }
        }
    }
}

} // namespace vsomeip
