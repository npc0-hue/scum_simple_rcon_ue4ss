#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <winsock2.h>

#include "Config.hpp"

namespace simple_rcon
{
    class RconServer
    {
      public:
        using CommandHandler = std::function<std::string(const std::string&)>;
        using Logger = std::function<void(const std::string&)>;

        RconServer(Config config, CommandHandler handler, Logger logger);
        ~RconServer();

        bool start();
        void stop();
        bool running() const;

      private:
        struct Packet
        {
            int32_t id{};
            int32_t type{};
            std::string body;
        };

        void accept_loop();
        void client_loop(SOCKET client, std::string remote);
        bool read_packet(SOCKET socket, Packet& out);
        bool send_packet(SOCKET socket, int32_t id, int32_t type, const std::string& body);
        bool send_response_chunks(SOCKET socket, int32_t id, const std::string& body);
        void close_client_socket(SOCKET socket);
        void log(const std::string& line);

        Config m_config;
        CommandHandler m_handler;
        Logger m_logger;
        std::atomic<bool> m_running{false};
        std::atomic<int> m_active_clients{0};
        std::thread m_accept_thread;
        SOCKET m_listen_socket{INVALID_SOCKET};
        std::mutex m_clients_mutex;
        std::unordered_set<SOCKET> m_clients;
        std::vector<std::thread> m_client_threads;
    };
}
