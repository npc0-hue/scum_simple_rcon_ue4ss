#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace simple_rcon
{
    struct Config
    {
        std::string bind_address{"127.0.0.1"};
        uint16_t port{27015};
        std::string password{"CHANGE_ME_BEFORE_USE"};
        bool auth_log{true};
        int max_connections{4};
        int packet_body_bytes{4000};
        std::chrono::milliseconds command_timeout{std::chrono::milliseconds{10000}};
        std::string dispatch_context{"auto"};
        bool offline_admin_dispatch{true};
        bool native_admin_executor{true};
        bool allow_empty_password{false};

        // Read from SCUM/Saved/Config/WindowsServer/AdminUsers.ini by default.
        // This prevents silently treating an arbitrary online player as an admin.
        std::string admin_users_file{"auto"};
        // Optional selector only; it is accepted only when also present in
        // admin_users_file and is never a replacement authorization source.
        std::string preferred_admin_steam_id{};
        bool require_configured_admin{true};
    };

    Config load_config(const std::filesystem::path& path);
}
