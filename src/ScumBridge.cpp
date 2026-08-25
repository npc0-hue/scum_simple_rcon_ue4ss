#include "ScumBridge.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <codecvt>
#include <fstream>
#include <locale>
#include <sstream>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace
{
    auto is_steam_id64(const std::string& value) -> bool
    {
        return value.size() == 17 &&
               std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    auto find_steam_ids_in_line(const std::string& line) -> std::vector<std::string>
    {
        std::vector<std::string> ids;
        for (size_t begin = 0; begin < line.size();)
        {
            if (!std::isdigit(static_cast<unsigned char>(line[begin])))
            {
                ++begin;
                continue;
            }

            size_t end = begin;
            while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
            {
                ++end;
            }
            if (end - begin == 17)
            {
                ids.emplace_back(line.substr(begin, 17));
            }
            begin = end;
        }
        return ids;
    }

    auto path_to_utf8(const std::filesystem::path& path) -> std::string
    {
        return path.string();
    }
}

namespace simple_rcon
{
    void ScumBridge::configure(const Config& config, const std::filesystem::path& mod_directory)
    {
        std::scoped_lock lock{m_mutex};
        m_require_configured_admin = config.require_configured_admin;
        m_preferred_admin_steam_id = config.preferred_admin_steam_id;
        m_admin_users_file = resolve_admin_users_file(config, mod_directory);
        m_admin_ids.clear();
        m_admin_file_seen = false;
        m_admin_users_last_write = {};
        m_offline_executor.configure(config.offline_admin_dispatch, config.native_admin_executor);
    }

    void ScumBridge::on_unreal_init()
    {
        std::scoped_lock lock{m_mutex};
        m_unreal_ready = true;
        m_offline_executor.on_unreal_init();
        refresh_admin_ids_locked();
        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[scum_simple_rcon] SCUM bridge ready; AdminUsers.ini: {} ({} IDs)\n"),
            m_admin_users_file.wstring(),
            m_admin_ids.size());
    }

    std::string ScumBridge::execute(const std::string& command)
    {
        const auto cmd = trim_command(command);
        if (cmd.empty())
        {
            return "error: empty command";
        }

        std::scoped_lock lock{m_mutex};
        if (!m_unreal_ready)
        {
            return "error: Unreal bridge not ready yet";
        }

        if (cmd == "__scum_simple_rcon_chat_probe__")
        {
            const auto chat = m_real_chat.probe();
            return chat.success ? chat.message : "error: " + chat.message;
        }

        // Ordinary SendChat is intentionally separate from admin-command
        // execution. It resolves only real, online target controllers and
        // does not borrow a PlayerRpcChannel or require an administrator ID.
        const auto chat = m_real_chat.dispatch_if_chat(cmd);
        if (chat.handled)
        {
            return chat.success ? chat.message : "error: " + chat.message;
        }

        refresh_admin_ids_locked();
        const std::string admin_id = selected_admin_id_locked();
        if (m_require_configured_admin && admin_id.empty())
        {
            return "error: no valid SteamID64 found in AdminUsers.ini: " + path_to_utf8(m_admin_users_file);
        }

        if (cmd == "__scum_simple_rcon_probe__")
        {
            const auto probe = m_offline_executor.probe(admin_id);
            return probe.success ? probe.message : "error: " + probe.message;
        }

        const auto offline = m_offline_executor.execute(cmd, admin_id);
        if (offline.attempted)
        {
            return offline.success ? offline.message : "error: " + offline.message;
        }

        std::string context_detail;
        auto* context = resolve_server_context(context_detail);
        if (!context)
        {
            return "error: offline dispatcher skipped (" + offline.message + ") and no server execution context: " + context_detail;
        }

        std::string dispatch_detail;
        if (!process_server_console(context, widen_utf8(cmd), dispatch_detail))
        {
            return "error: offline dispatcher skipped (" + offline.message + ") and server console dispatch failed: " + dispatch_detail;
        }

        return "ok: server-console-dispatched '" + cmd + "'" +
               (admin_id.empty() ? std::string{} : " (configured admin " + admin_id + ")");
    }

    std::string ScumBridge::admin_status()
    {
        std::scoped_lock lock{m_mutex};
        refresh_admin_ids_locked();

        std::ostringstream reply;
        reply << "AdminUsers.ini=" << path_to_utf8(m_admin_users_file)
              << "; configured_admins=" << m_admin_ids.size();
        const auto selected = selected_admin_id_locked();
        if (!selected.empty())
        {
            reply << "; selected=" << selected;
        }
        return reply.str();
    }

    std::string ScumBridge::dispatch_status()
    {
        std::scoped_lock lock{m_mutex};
        return m_offline_executor.status();
    }

    RC::Unreal::UObject* ScumBridge::resolve_server_context(std::string& detail)
    {
        // These are server-owned objects and exist independently of connected
        // players. FindFirstOf deliberately ignores class-default objects.
        if (auto* game_mode = RC::Unreal::UObjectGlobals::FindFirstOf(STR("GameModeBase")))
        {
            detail = "GameModeBase";
            return game_mode;
        }
        if (auto* game_instance = RC::Unreal::UObjectGlobals::FindFirstOf(STR("GameInstance")))
        {
            detail = "GameInstance";
            return game_instance;
        }
        if (auto* world = RC::Unreal::UObjectGlobals::FindFirstOf(STR("World")))
        {
            detail = "World";
            return world;
        }

        detail = "GameModeBase, GameInstance, and World are not available yet";
        return nullptr;
    }

    bool ScumBridge::process_server_console(RC::Unreal::UObject* context,
                                             const std::wstring& command,
                                             std::string& detail)
    {
        RC::Unreal::FOutputDevice output{};
        if (context->ProcessConsoleExec(command.c_str(), output, context))
        {
            detail = "ProcessConsoleExec returned true";
            return true;
        }

        // Fallback: UE's string invocation path can resolve command-like UFUNCTIONs
        // that do not respond through ProcessConsoleExec on the first context.
        if (RC::Unreal::UObject::CallFunctionByNameWithArgumentsInternal(context, command.c_str(), output, context, true))
        {
            detail = "CallFunctionByNameWithArgumentsInternal returned true";
            return true;
        }

        detail = "ProcessConsoleExec and CallFunctionByNameWithArgumentsInternal both returned false";
        return false;
    }

    void ScumBridge::refresh_admin_ids_locked()
    {
        if (m_admin_users_file.empty())
        {
            return;
        }

        std::error_code ec;
        const bool exists = std::filesystem::exists(m_admin_users_file, ec);
        if (ec || !exists)
        {
            if (m_admin_file_seen)
            {
                m_admin_ids.clear();
                m_admin_file_seen = false;
            }
            return;
        }

        const auto last_write = std::filesystem::last_write_time(m_admin_users_file, ec);
        if (ec || (m_admin_file_seen && last_write == m_admin_users_last_write))
        {
            return;
        }

        std::ifstream file{m_admin_users_file};
        if (!file)
        {
            return;
        }

        std::unordered_set<std::string> next;
        std::string line;
        while (std::getline(file, line))
        {
            const auto comment = line.find_first_of(";#");
            if (comment != std::string::npos)
            {
                line.resize(comment);
            }
            for (const auto& id : find_steam_ids_in_line(line))
            {
                next.insert(id);
            }
        }

        m_admin_ids = std::move(next);
        m_admin_users_last_write = last_write;
        m_admin_file_seen = true;
    }

    std::string ScumBridge::selected_admin_id_locked() const
    {
        if (is_steam_id64(m_preferred_admin_steam_id) && m_admin_ids.contains(m_preferred_admin_steam_id))
        {
            return m_preferred_admin_steam_id;
        }

        if (m_admin_ids.empty())
        {
            return {};
        }

        std::vector<std::string> ordered{m_admin_ids.begin(), m_admin_ids.end()};
        std::sort(ordered.begin(), ordered.end());
        return ordered.front();
    }

    std::filesystem::path ScumBridge::resolve_admin_users_file(const Config& config,
                                                                 const std::filesystem::path& mod_directory)
    {
        if (!config.admin_users_file.empty() && config.admin_users_file != "auto")
        {
            const std::filesystem::path configured{config.admin_users_file};
            return configured.is_absolute() ? configured : mod_directory / configured;
        }

        // <SCUM>/Binaries/Win64/ue4ss/Mods/scum_simple_rcon
        const auto win64 = mod_directory.parent_path().parent_path().parent_path();
        const auto scum_root = win64.parent_path().parent_path();
        return scum_root / "Saved" / "Config" / "WindowsServer" / "AdminUsers.ini";
    }

    std::wstring ScumBridge::widen_utf8(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        return conv.from_bytes(s);
    }

    std::string ScumBridge::trim_command(std::string s)
    {
        if (!s.empty() && s[0] == '#')
        {
            s.erase(s.begin());
        }
        const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    }
}
