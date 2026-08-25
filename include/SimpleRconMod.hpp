#pragma once

#include <memory>
#include <string>

#include <Mod/CppUserModBase.hpp>

#include "CommandQueue.hpp"
#include "Config.hpp"
#include "RconServer.hpp"
#include "ScumBridge.hpp"

namespace simple_rcon
{
    class SimpleRconMod final : public RC::CppUserModBase
    {
      public:
        SimpleRconMod();
        ~SimpleRconMod() override;

        auto on_unreal_init() -> void override;
        auto on_update() -> void override;

      private:
        void log(const std::string& line);
        std::string handle_rcon_command(const std::string& command);
        void install_game_thread_drain();
        void drain_game_thread();

        Config m_config;
        ScumBridge m_bridge;
        CommandQueue m_queue;
        std::unique_ptr<RconServer> m_server;
        bool m_hook_installed{false};
        uint64_t m_tick_callback_id{0};
    };
}
