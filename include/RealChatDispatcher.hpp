#pragma once

#include <string>
#include <string_view>

namespace simple_rcon
{
    /**
     * Handles ordinary SCUM chat independently of AdminCommand dispatch.
     *
     * It never creates or picks an arbitrary PlayerRpcChannel.  A targeted
     * chat message is sent only after a configured SteamID is matched against
     * a real, currently network-backed ConZPlayerController whose Player is a
     * live UNetConnection. Broadcast chat is the same operation performed
     * once for each such controller.
     */
    class RealChatDispatcher
    {
      public:
        struct Result
        {
            bool handled{false};
            bool success{false};
            std::string message;
        };

        RealChatDispatcher();
        ~RealChatDispatcher();
        RealChatDispatcher(const RealChatDispatcher&) = delete;
        RealChatDispatcher& operator=(const RealChatDispatcher&) = delete;

        // Recognizes: SendChat <type 0-7> "message" [target SteamID64]
        // This function must be called on the game thread.
        Result dispatch_if_chat(std::string_view command);
        // Non-mutating game-thread diagnostic for the real-player chat path.
        Result probe();
        std::string status() const;

        // Public only so the game-thread helper functions in the .cpp can
        // keep all Unreal implementation details out of this header.
        struct RuntimeState;

      private:
        RuntimeState* m_state{nullptr};
    };
}
