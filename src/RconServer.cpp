#include "RconServer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <vector>

#include <ws2tcpip.h>

namespace
{
    constexpr int32_t SERVERDATA_RESPONSE_VALUE = 0;
    constexpr int32_t SERVERDATA_AUTH_RESPONSE = 2;
    constexpr int32_t SERVERDATA_EXECCOMMAND = 2;
    constexpr int32_t SERVERDATA_AUTH = 3;
    constexpr int32_t MAX_PACKET_SIZE = 4096;

    bool send_all(SOCKET s, const char* data, int len)
    {
        int sent_total = 0;
        while (sent_total < len)
        {
            const int sent = ::send(s, data + sent_total, len - sent_total, 0);
            if (sent <= 0)
            {
                return false;
            }
            sent_total += sent;
        }
        return true;
    }

    bool recv_all(SOCKET s, char* data, int len)
    {
        int got_total = 0;
        while (got_total < len)
        {
            const int got = ::recv(s, data + got_total, len - got_total, 0);
            if (got <= 0)
            {
                return false;
            }
            got_total += got;
        }
        return true;
    }

    template <typename T>
    T read_le(const char* p)
    {
        T value{};
        std::memcpy(&value, p, sizeof(T));
        return value;
    }

    template <typename T>
    void append_le(std::vector<char>& out, T value)
    {
        const auto* p = reinterpret_cast<const char*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }
}

namespace simple_rcon
{
    RconServer::RconServer(Config config, CommandHandler handler, Logger logger)
        : m_config(std::move(config)), m_handler(std::move(handler)), m_logger(std::move(logger))
    {
    }

    RconServer::~RconServer()
    {
        stop();
    }

    bool RconServer::start()
    {
        if (m_running.exchange(true))
        {
            return true;
        }

        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            m_running = false;
            log("rcon: WSAStartup failed");
            return false;
        }

        m_listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen_socket == INVALID_SOCKET)
        {
            m_running = false;
            log("rcon: socket() failed");
            ::WSACleanup();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(m_config.port);
        if (::inet_pton(AF_INET, m_config.bind_address.c_str(), &addr.sin_addr) != 1)
        {
            log("rcon: invalid bind_address '" + m_config.bind_address + "'");
            stop();
            return false;
        }

        int yes = 1;
        ::setsockopt(m_listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

        if (::bind(m_listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            log("rcon: bind failed on " + m_config.bind_address + ":" + std::to_string(m_config.port));
            stop();
            return false;
        }

        if (::listen(m_listen_socket, SOMAXCONN) == SOCKET_ERROR)
        {
            log("rcon: listen() failed");
            stop();
            return false;
        }

        m_accept_thread = std::thread([this] { accept_loop(); });
        log("rcon: listening on " + m_config.bind_address + ":" + std::to_string(m_config.port));
        return true;
    }

    void RconServer::stop()
    {
        if (!m_running.exchange(false) && m_listen_socket == INVALID_SOCKET)
        {
            return;
        }

        if (m_listen_socket != INVALID_SOCKET)
        {
            ::shutdown(m_listen_socket, SD_BOTH);
            ::closesocket(m_listen_socket);
            m_listen_socket = INVALID_SOCKET;
        }

        if (m_accept_thread.joinable())
        {
            m_accept_thread.join();
        }

        std::vector<std::thread> client_threads;
        {
            std::scoped_lock lock{m_clients_mutex};
            for (SOCKET client : m_clients)
            {
                // shutdown unblocks recv; the owner thread closes the socket.
                ::shutdown(client, SD_BOTH);
            }
            std::swap(client_threads, m_client_threads);
        }

        for (auto& client_thread : client_threads)
        {
            if (client_thread.joinable())
            {
                client_thread.join();
            }
        }

        ::WSACleanup();
    }

    bool RconServer::running() const
    {
        return m_running.load();
    }

    void RconServer::accept_loop()
    {
        while (m_running)
        {
            sockaddr_in remote_addr{};
            int remote_len = sizeof(remote_addr);
            SOCKET client = ::accept(m_listen_socket, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len);
            if (client == INVALID_SOCKET)
            {
                if (m_running)
                {
                    log("rcon: accept() failed");
                }
                continue;
            }

            char ip[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &remote_addr.sin_addr, ip, sizeof(ip));
            std::ostringstream remote;
            remote << ip << ":" << ntohs(remote_addr.sin_port);

            if (m_active_clients.fetch_add(1) >= m_config.max_connections)
            {
                --m_active_clients;
                log("rcon: connection limit reached; refusing " + remote.str());
                ::closesocket(client);
                continue;
            }

            {
                std::scoped_lock lock{m_clients_mutex};
                m_clients.insert(client);
            }

            std::scoped_lock lock{m_clients_mutex};
            m_client_threads.emplace_back([this, client, remote = remote.str()] {
                client_loop(client, remote);
                close_client_socket(client);
            });
        }
    }

    void RconServer::client_loop(SOCKET client, std::string remote)
    {
        bool authed = false;

        int timeout_ms = 30000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        Packet packet{};
        while (m_running && read_packet(client, packet))
        {
            if (!authed)
            {
                if (packet.type != SERVERDATA_AUTH)
                {
                    log("rcon: " + remote + " sent command before auth");
                    send_packet(client, -1, SERVERDATA_AUTH_RESPONSE, "");
                    break;
                }

                const bool ok = packet.body == m_config.password &&
                                (m_config.allow_empty_password || !m_config.password.empty()) &&
                                (m_config.allow_empty_password || m_config.password != "CHANGE_ME_BEFORE_USE");

                if (!ok)
                {
                    if (m_config.auth_log)
                    {
                        log("rcon: auth FAILED from " + remote);
                    }
                    send_packet(client, -1, SERVERDATA_AUTH_RESPONSE, "");
                    break;
                }

                authed = true;
                if (m_config.auth_log)
                {
                    log("rcon: auth succeeded from " + remote);
                }
                send_packet(client, packet.id, SERVERDATA_AUTH_RESPONSE, "");
                continue;
            }

            if (packet.type != SERVERDATA_EXECCOMMAND)
            {
                send_response_chunks(client, packet.id, "error: unsupported packet type " + std::to_string(packet.type));
                continue;
            }

            log("rcon: " + remote + " -> " + packet.body);
            const std::string response = m_handler ? m_handler(packet.body) : "error: no command handler installed";
            if (!send_response_chunks(client, packet.id, response))
            {
                break;
            }
        }

        --m_active_clients;
    }

    bool RconServer::read_packet(SOCKET socket, Packet& out)
    {
        std::array<char, 4> size_buf{};
        if (!recv_all(socket, size_buf.data(), static_cast<int>(size_buf.size())))
        {
            return false;
        }

        const int32_t size = read_le<int32_t>(size_buf.data());
        if (size < 10 || size > MAX_PACKET_SIZE)
        {
            return false;
        }

        std::vector<char> buf(static_cast<size_t>(size));
        if (!recv_all(socket, buf.data(), size))
        {
            return false;
        }

        out.id = read_le<int32_t>(buf.data());
        out.type = read_le<int32_t>(buf.data() + 4);
        const char* body = buf.data() + 8;
        const int body_capacity = size - 10;
        out.body.assign(body, body + std::max(0, body_capacity));
        const auto nul = out.body.find('\0');
        if (nul != std::string::npos)
        {
            out.body.resize(nul);
        }
        return true;
    }

    bool RconServer::send_packet(SOCKET socket, int32_t id, int32_t type, const std::string& body)
    {
        const int32_t size = static_cast<int32_t>(8 + body.size() + 2);
        std::vector<char> packet;
        packet.reserve(static_cast<size_t>(size) + 4);
        append_le<int32_t>(packet, size);
        append_le<int32_t>(packet, id);
        append_le<int32_t>(packet, type);
        packet.insert(packet.end(), body.begin(), body.end());
        packet.push_back('\0');
        packet.push_back('\0');
        return send_all(socket, packet.data(), static_cast<int>(packet.size()));
    }

    bool RconServer::send_response_chunks(SOCKET socket, int32_t id, const std::string& body)
    {
        const int max_body = std::clamp(m_config.packet_body_bytes, 1, 4000);
        if (body.empty())
        {
            return send_packet(socket, id, SERVERDATA_RESPONSE_VALUE, "");
        }

        size_t offset = 0;
        while (offset < body.size())
        {
            const size_t n = std::min<size_t>(static_cast<size_t>(max_body), body.size() - offset);
            if (!send_packet(socket, id, SERVERDATA_RESPONSE_VALUE, body.substr(offset, n)))
            {
                return false;
            }
            offset += n;
        }

        // A final empty response packet is the common Source-RCON sentinel
        // that lets clients stop waiting after a multi-packet reply.
        return send_packet(socket, id, SERVERDATA_RESPONSE_VALUE, "");
    }

    void RconServer::close_client_socket(SOCKET socket)
    {
        {
            std::scoped_lock lock{m_clients_mutex};
            m_clients.erase(socket);
        }
        ::shutdown(socket, SD_BOTH);
        ::closesocket(socket);
    }

    void RconServer::log(const std::string& line)
    {
        if (m_logger)
        {
            m_logger(line);
        }
    }
}
