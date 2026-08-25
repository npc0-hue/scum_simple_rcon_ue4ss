#pragma once

#include <string>
#include <string_view>

namespace simple_rcon
{
    /**
     * SCUM's normal admin commands are not Unreal console commands.  They are
     * instances of AdminCommand executed by a native server function.  This
     * class creates a server-only ConZPlayerController/ConZCharacter pair and
     * calls that function, so an actual player account does not need to be
     * connected.
     *
     * It is deliberately game-build guarded: if the known executor signature
     * does not resolve to exactly one location, no native call is made.
     */
    class OfflineAdminExecutor
    {
      public:
        // Kept public solely so the .cpp's game-thread helper functions can
        // operate on it without exposing any Unreal types in this header.
        struct RuntimeState;

        OfflineAdminExecutor();
        ~OfflineAdminExecutor();
        OfflineAdminExecutor(const OfflineAdminExecutor&) = delete;
        OfflineAdminExecutor& operator=(const OfflineAdminExecutor&) = delete;

        struct Result
        {
            bool attempted{false};
            bool success{false};
            std::string message;
        };

        void configure(bool enabled, bool native_executor_enabled);
        void on_unreal_init();
        // Runs the non-mutating portion of the offline path on the game
        // thread: discover command classes, create/reuse the synthetic caller,
        // and resolve the guarded native executor address.
        Result probe(std::string_view configured_admin_steam_id);
        Result execute(std::string_view command, std::string_view configured_admin_steam_id);
        std::string status() const;

      private:
        bool m_enabled{true};
        bool m_native_executor_enabled{true};
        bool m_unreal_ready{false};
        RuntimeState* m_state{nullptr};
    };
}
