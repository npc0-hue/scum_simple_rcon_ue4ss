#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "Config.hpp"
#include "OfflineAdminExecutor.hpp"
#include "RealChatDispatcher.hpp"

namespace RC::Unreal
{
    class UObject;
}

namespace simple_rcon
{
    class ScumBridge
    {
      public:
        void configure(const Config& config, const std::filesystem::path& mod_directory);
        void on_unreal_init();
        std::string execute(const std::string& command);
        std::string admin_status();
        std::string dispatch_status();

      private:
        RC::Unreal::UObject* resolve_server_context(std::string& detail);
        bool process_server_console(RC::Unreal::UObject* context, const std::wstring& command, std::string& detail);
        void refresh_admin_ids_locked();
        std::string selected_admin_id_locked() const;
        static std::filesystem::path resolve_admin_users_file(const Config& config, const std::filesystem::path& mod_directory);
        static std::wstring widen_utf8(const std::string& s);
        static std::string trim_command(std::string s);

        std::mutex m_mutex;
        bool m_unreal_ready{false};
        bool m_require_configured_admin{true};
        std::string m_preferred_admin_steam_id;
        std::filesystem::path m_admin_users_file;
        std::filesystem::file_time_type m_admin_users_last_write{};
        bool m_admin_file_seen{false};
        std::unordered_set<std::string> m_admin_ids;
        OfflineAdminExecutor m_offline_executor;
        RealChatDispatcher m_real_chat;
    };
}
