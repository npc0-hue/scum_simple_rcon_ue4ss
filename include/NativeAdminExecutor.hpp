#pragma once

#include <string>
#include <string_view>

namespace simple_rcon
{
    /**
     * Executes registered SCUM AdminCommand objects through their native
     * virtual method. It never creates a controller or pawn: the caller must
     * name an exact connected player so SCUM receives the same real
     * ConZPlayerController/Prisoner context as an in-game administrator.
     */
    class NativeAdminExecutor
    {
      public:
        struct RuntimeState;

        NativeAdminExecutor();
        ~NativeAdminExecutor();
        NativeAdminExecutor(const NativeAdminExecutor&) = delete;
        NativeAdminExecutor& operator=(const NativeAdminExecutor&) = delete;

        struct Result
        {
            bool attempted{false};
            bool success{false};
            std::string message;
        };

        void configure(bool enabled, bool native_executor_enabled);
        void on_unreal_init();

        // Inspects the live registry and verifies that the current build's
        // native executor can be identified exactly. No player is required.
        Result probe();
        // Recognizes: !exec <SteamID64> <native command> [arguments]
        // Must run on the game thread. Unprefixed commands are not handled.
        Result execute(std::string_view command);
        std::string status() const;

      private:
        bool m_enabled{true};
        bool m_native_executor_enabled{true};
        bool m_unreal_ready{false};
        RuntimeState* m_state{nullptr};
    };
}
