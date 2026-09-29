#include "SimpleRconMod.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <UE4SSRuntime.hpp>
#include <Unreal/Hooks/Hooks.hpp>

namespace
{
    std::string trim(std::string s)
    {
        auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    }

    std::filesystem::path mod_dir()
    {
        HMODULE module{};
        wchar_t path[MAX_PATH]{};
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&mod_dir),
                                  &module) &&
            ::GetModuleFileNameW(module, path, MAX_PATH) > 0)
        {
            auto p = std::filesystem::path(path);
            // .../Mods/scum_simple_rcon/dlls/main.dll -> .../Mods/scum_simple_rcon
            return p.parent_path().parent_path();
        }
        return std::filesystem::current_path() / "ue4ss" / "Mods" / "scum_simple_rcon";
    }
}

namespace simple_rcon
{
    Config load_config(const std::filesystem::path& path)
    {
        Config cfg{};
        std::ifstream in(path);
        if (!in)
        {
            return cfg;
        }

        std::string section;
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
            {
                continue;
            }
            if (line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            try
            {
                if (section == "rcon")
                {
                    if (key == "bind_address") cfg.bind_address = value;
                    else if (key == "port")
                    {
                        const int port = std::stoi(value);
                        if (port >= 1 && port <= 65535) cfg.port = static_cast<uint16_t>(port);
                    }
                    else if (key == "password") cfg.password = value;
                    else if (key == "auth_log") cfg.auth_log = value == "true" || value == "1";
                    else if (key == "max_connections") cfg.max_connections = std::clamp(std::stoi(value), 1, 32);
                    else if (key == "packet_body_bytes") cfg.packet_body_bytes = std::clamp(std::stoi(value), 1, 4000);
                    else if (key == "command_timeout_ms")
                    {
                        cfg.command_timeout = std::chrono::milliseconds{std::clamp(std::stoi(value), 100, 60000)};
                    }
                    else if (key == "allow_empty_password") cfg.allow_empty_password = value == "true" || value == "1";
                }
                else if (section == "dispatch")
                {
                    if (key == "context") cfg.dispatch_context = value;
                    else if (key == "offline_admin_dispatch") cfg.offline_admin_dispatch = value == "true" || value == "1";
                    else if (key == "native_admin_executor") cfg.native_admin_executor = value == "true" || value == "1";
                }
                else if (section == "admin")
                {
                    if (key == "users_file") cfg.admin_users_file = value;
                    else if (key == "preferred_steam_id") cfg.preferred_admin_steam_id = value;
                    else if (key == "require_configured_admin") cfg.require_configured_admin = value == "true" || value == "1";
                }
            }
            catch (const std::exception&)
            {
                // Ignore an invalid individual value and retain the safe default.
            }
        }
        return cfg;
    }

    SimpleRconMod::SimpleRconMod()
        : m_config(load_config(mod_dir() / "config.ini")),
          m_queue([this](const std::string& command) { return m_bridge.execute(command); })
    {
        ModName = STR("scum_simple_rcon");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Minimal Source RCON server for SCUM via UE4SS");
        ModAuthors = STR("Codex");
        m_bridge.configure(m_config, mod_dir());
    }

    SimpleRconMod::~SimpleRconMod()
    {
        if (m_server)
        {
            m_server->stop();
        }
        m_queue.shutdown();
        if (m_tick_callback_id != 0)
        {
            RC::Unreal::Hook::UnregisterCallback(m_tick_callback_id);
        }
    }

    auto SimpleRconMod::on_unreal_init() -> void
    {
        m_bridge.on_unreal_init();
        install_game_thread_drain();

        if (!m_config.allow_empty_password && (m_config.password.empty() || m_config.password == "CHANGE_ME_BEFORE_USE"))
        {
            log("rcon: listener NOT started; set [rcon] password in config.ini");
            return;
        }

        m_server = std::make_unique<RconServer>(
            m_config,
            [this](const std::string& command) { return handle_rcon_command(command); },
            [this](const std::string& line) { log(line); });

        if (!m_server->start())
        {
            log("rcon: listener failed to start");
        }
    }

    auto SimpleRconMod::on_update() -> void
    {
        // Fallback path if EngineTick hook is unavailable in a given UE4SS build.
        if (!m_hook_installed)
        {
            drain_game_thread();
        }
    }

    void SimpleRconMod::install_game_thread_drain()
    {
        if (m_hook_installed)
        {
            return;
        }

        if (!RC::UE4SSRuntime::IsEngineTickAvailable())
        {
            log("game-thread drain: EngineTick unavailable; falling back to on_update");
            return;
        }

        // FCallbackOptions is {bOnce, bReadonly, OwnerModName, HookName}.
        // bOnce must stay false: this callback has to run on every engine tick.
        // bOnce=true lets UE4SS claim the callback on its first invocation and
        // the garbage collector prunes it, which silently strands every queued
        // RCON command with no game-thread consumer left to drain it.
        m_tick_callback_id = RC::Unreal::Hook::RegisterEngineTickPreCallback(
            [this](auto&, RC::Unreal::UEngine*, float, bool) {
                drain_game_thread();
            },
            {false, true, STR("scum_simple_rcon"), STR("DrainCommands")});

        m_hook_installed = m_tick_callback_id != 0;
        log(m_hook_installed ? "game-thread drain installed via EngineTick" : "game-thread drain install failed");
    }

    void SimpleRconMod::drain_game_thread()
    {
        m_drain_calls.fetch_add(1, std::memory_order_relaxed);
        m_queue.drain(32);
    }

    std::string SimpleRconMod::handle_rcon_command(const std::string& command)
    {
        const auto cmd = trim(command);
        if (cmd.empty())
        {
            return "error: empty command";
        }

        if (cmd == "rcon.status")
        {
            const auto drains = m_drain_calls.load(std::memory_order_relaxed);
            if (!m_hook_installed)
            {
                return "scum_simple_rcon: DEGRADED; no EngineTick hook; drain runs on the UE4SS update thread";
            }
            if (drains == 0)
            {
                return "scum_simple_rcon: DEGRADED; EngineTick hook installed but has never fired; queued commands will time out";
            }
            return "scum_simple_rcon: ok; game-thread queue active; drains=" + std::to_string(drains);
        }
        if (cmd == "rcon.help")
        {
            return "internal: rcon.status, rcon.admins, rcon.dispatch, rcon.chat, rcon.help; SendChat uses real online controllers; otherwise send raw SCUM command text";
        }
        if (cmd == "rcon.admins")
        {
            return m_bridge.admin_status();
        }
        if (cmd == "rcon.dispatch")
        {
            // A dispatcher probe creates UObject state and scans the game
            // executable, so it must take the exact same game-thread path as
            // an actual RCON command.
            return m_queue.enqueue_and_wait("__scum_simple_rcon_probe__", m_config.command_timeout);
        }
        if (cmd == "rcon.chat")
        {
            return m_queue.enqueue_and_wait("__scum_simple_rcon_chat_probe__", m_config.command_timeout);
        }

        return m_queue.enqueue_and_wait(cmd, m_config.command_timeout);
    }

    void SimpleRconMod::log(const std::string& line)
    {
        const std::wstring wide(line.begin(), line.end());
        RC::Output::send<RC::LogLevel::Verbose>(STR("[scum_simple_rcon] {}\n"), wide);
    }
}
