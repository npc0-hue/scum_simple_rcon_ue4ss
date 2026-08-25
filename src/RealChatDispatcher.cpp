#include "RealChatDispatcher.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <Windows.h>

#include <UnrealDef.hpp>

namespace
{
    using RC::Unreal::FProperty;
    using RC::Unreal::FString;
    using RC::Unreal::UClass;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;

    constexpr uint16_t k_no_return_value = 0xFFFF;
    constexpr size_t k_max_message_bytes = 1024;

    auto lowercase(std::string value) -> std::string
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    auto narrow(const std::wstring& input) -> std::string
    {
        std::string output;
        output.reserve(input.size());
        for (wchar_t c : input)
        {
            output.push_back(c >= 0 && c <= 0x7F ? static_cast<char>(c) : '?');
        }
        return output;
    }

    auto fstring_to_utf8(FString& value) -> std::string
    {
        const auto* characters = *value;
        return characters ? narrow(std::wstring{characters}) : std::string{};
    }

    auto widen_utf8(std::string_view input) -> std::wstring
    {
        if (input.empty())
        {
            return {};
        }

        const int characters = ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
        if (characters <= 0)
        {
            return {input.begin(), input.end()};
        }

        std::wstring output(static_cast<size_t>(characters), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), characters);
        return output;
    }

    auto tokenize(std::string_view command) -> std::vector<std::string>
    {
        std::vector<std::string> result;
        std::string current;
        bool quoted = false;
        bool escaped = false;

        for (char c : command)
        {
            if (escaped)
            {
                current.push_back(c);
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '"')
            {
                quoted = !quoted;
                continue;
            }
            if (!quoted && std::isspace(static_cast<unsigned char>(c)))
            {
                if (!current.empty())
                {
                    result.emplace_back(std::move(current));
                    current.clear();
                }
                continue;
            }
            current.push_back(c);
        }

        if (escaped)
        {
            current.push_back('\\');
        }
        if (!current.empty())
        {
            result.emplace_back(std::move(current));
        }
        return result;
    }

    auto is_steam_id64(std::string_view value) -> bool
    {
        return value.size() == 17 &&
               std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    template <typename T>
    auto find_object(const wchar_t* path) -> T*
    {
        return RC::Unreal::UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
    }

    auto class_is_child_of(UClass* candidate, UClass* expected_base) -> bool
    {
        for (auto* current = candidate; current; current = static_cast<UClass*>(current->GetSuperStruct()))
        {
            if (current == expected_base)
            {
                return true;
            }
        }
        return false;
    }

    auto find_property(UClass* klass, const wchar_t* name) -> FProperty*
    {
        for (auto* current = klass; current; current = static_cast<UClass*>(current->GetSuperStruct()))
        {
            for (FProperty* property : RC::Unreal::TFieldRange<FProperty>(current, RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
            {
                if (property && property->GetName() == name)
                {
                    return property;
                }
            }
        }
        return nullptr;
    }

    auto is_parameter(FProperty* property) -> bool
    {
        return property && property->HasAnyPropertyFlags(RC::Unreal::EPropertyFlags::CPF_Parm);
    }

    auto property_name(FProperty* property) -> std::string
    {
        return property ? narrow(property->GetName()) : std::string{};
    }

    auto property_description(FProperty* property) -> std::string
    {
        if (!property)
        {
            return "<null>";
        }
        return property_name(property) + ":" + narrow(property->GetClass().GetName());
    }

    auto is_chat_type_property(FProperty* property) -> bool
    {
        return property &&
               (property->IsA<RC::Unreal::FByteProperty>() ||
                property->IsA<RC::Unreal::FIntProperty>() ||
                property->IsA<RC::Unreal::FInt64Property>() ||
                property->IsA<RC::Unreal::FUInt64Property>() ||
                property->IsA<RC::Unreal::FEnumProperty>());
    }

    class FunctionParameterBuffer
    {
      public:
        explicit FunctionParameterBuffer(UFunction* function)
        {
            const size_t byte_count = function ? std::max<size_t>(function->GetPropertiesSize(), 1) : 0;
            m_byte_capacity = byte_count;
            if (byte_count == 0)
            {
                return;
            }

            m_storage = static_cast<uint8_t*>(::operator new(byte_count, std::align_val_t{alignof(std::max_align_t)}));
            std::memset(m_storage, 0, byte_count);

            if (!function)
            {
                return;
            }

            for (FProperty* property : RC::Unreal::TFieldRange<FProperty>(function, RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
            {
                if (!is_parameter(property) || !property->IsA<RC::Unreal::FStrProperty>())
                {
                    continue;
                }
                const auto offset = property->GetOffset_Internal();
                if (!contains(offset, sizeof(FString)))
                {
                    m_valid = false;
                    continue;
                }
                auto* field = reinterpret_cast<FString*>(bytes() + offset);
                new (field) FString{};
                m_strings.emplace_back(field);
            }
        }

        ~FunctionParameterBuffer()
        {
            for (auto* value : m_strings)
            {
                value->~FString();
            }
            ::operator delete(m_storage, std::align_val_t{alignof(std::max_align_t)});
        }

        FunctionParameterBuffer(const FunctionParameterBuffer&) = delete;
        FunctionParameterBuffer& operator=(const FunctionParameterBuffer&) = delete;

        auto valid() const -> bool { return m_valid && m_storage != nullptr; }
        auto data() -> void* { return m_storage; }

        auto write_object(FProperty* property, UObject* value) -> bool
        {
            if (!property || !contains(property->GetOffset_Internal(), sizeof(value)))
            {
                return false;
            }
            std::memcpy(bytes() + property->GetOffset_Internal(), &value, sizeof(value));
            return true;
        }

        auto write_string(FProperty* property, std::string_view value) -> bool
        {
            if (!property || !property->IsA<RC::Unreal::FStrProperty>() || !contains(property->GetOffset_Internal(), sizeof(FString)))
            {
                return false;
            }
            auto* field = reinterpret_cast<FString*>(bytes() + property->GetOffset_Internal());
            const auto wide = widen_utf8(value);
            *field = FString{wide.c_str()};
            return true;
        }

        auto write_small_integer(FProperty* property, int64_t value) -> bool
        {
            if (!property || !is_chat_type_property(property))
            {
                return false;
            }

            const auto offset = property->GetOffset_Internal();
            switch (property->GetSize())
            {
            case 1: {
                const auto written = static_cast<uint8_t>(value);
                if (!contains(offset, sizeof(written))) return false;
                std::memcpy(bytes() + offset, &written, sizeof(written));
                return true;
            }
            case 2: {
                const auto written = static_cast<uint16_t>(value);
                if (!contains(offset, sizeof(written))) return false;
                std::memcpy(bytes() + offset, &written, sizeof(written));
                return true;
            }
            case 4: {
                const auto written = static_cast<uint32_t>(value);
                if (!contains(offset, sizeof(written))) return false;
                std::memcpy(bytes() + offset, &written, sizeof(written));
                return true;
            }
            case 8: {
                const auto written = static_cast<uint64_t>(value);
                if (!contains(offset, sizeof(written))) return false;
                std::memcpy(bytes() + offset, &written, sizeof(written));
                return true;
            }
            default:
                return false;
            }
        }

        auto read_string(FProperty* property) const -> std::optional<std::string>
        {
            if (!property || !property->IsA<RC::Unreal::FStrProperty>() || !contains(property->GetOffset_Internal(), sizeof(FString)))
            {
                return std::nullopt;
            }
            auto* field = reinterpret_cast<FString*>(const_cast<uint8_t*>(bytes() + property->GetOffset_Internal()));
            return fstring_to_utf8(*field);
        }

      private:
        auto bytes() -> uint8_t* { return m_storage; }
        auto bytes() const -> const uint8_t* { return m_storage; }

        auto contains(int32_t offset, size_t size) const -> bool
        {
            return offset >= 0 && static_cast<size_t>(offset) + size <= m_byte_capacity;
        }

        uint8_t* m_storage{};
        std::vector<FString*> m_strings;
        size_t m_byte_capacity{};
        bool m_valid{true};
    };

    struct ChatSchema
    {
        FProperty* target_controller{};
        FProperty* world_context{};
        FProperty* message{};
        FProperty* message_type{};
        std::string detail;
    };

    auto discover_chat_schema(UFunction* function, ChatSchema& schema) -> bool
    {
        if (!function)
        {
            schema.detail = "SendChatLineToPlayer UFunction is not available";
            return false;
        }

        const uint16_t return_offset = function->GetReturnValueOffset();
        std::vector<FProperty*> objects;
        std::vector<FProperty*> strings;
        std::vector<FProperty*> numeric;
        std::vector<std::string> unsupported;

        for (FProperty* property : RC::Unreal::TFieldRange<FProperty>(function, RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
        {
            if (!is_parameter(property) || property->GetOffset_Internal() == return_offset)
            {
                continue;
            }

            const auto name = lowercase(property_name(property));
            if (property->IsA<RC::Unreal::FObjectProperty>())
            {
                if (name.find("worldcontext") != std::string::npos)
                {
                    if (schema.world_context)
                    {
                        unsupported.emplace_back(property_description(property));
                    }
                    else
                    {
                        schema.world_context = property;
                    }
                }
                else
                {
                    objects.emplace_back(property);
                }
            }
            else if (property->IsA<RC::Unreal::FStrProperty>())
            {
                strings.emplace_back(property);
            }
            else if (is_chat_type_property(property))
            {
                numeric.emplace_back(property);
            }
            else
            {
                unsupported.emplace_back(property_description(property));
            }
        }

        if (objects.size() == 1) schema.target_controller = objects.front();
        if (strings.size() == 1) schema.message = strings.front();
        if (numeric.size() == 1) schema.message_type = numeric.front();

        if (schema.target_controller && schema.message && schema.message_type && unsupported.empty() &&
            objects.size() == 1 && strings.size() == 1 && numeric.size() == 1)
        {
            schema.detail = "schema=" + property_description(schema.target_controller) + "," +
                            property_description(schema.message) + "," +
                            property_description(schema.message_type);
            return true;
        }

        std::ostringstream output;
        output << "unsupported SendChatLineToPlayer parameter schema";
        for (FProperty* property : objects) output << "; object=" << property_description(property);
        for (FProperty* property : strings) output << "; string=" << property_description(property);
        for (FProperty* property : numeric) output << "; numeric=" << property_description(property);
        for (const auto& property : unsupported) output << "; unsupported=" << property;
        schema.detail = output.str();
        return false;
    }

    auto controller_has_live_network_player(UObject* controller, UClass* net_connection_base) -> bool
    {
        if (!controller || !controller->GetClassPrivate() || !net_connection_base)
        {
            return false;
        }

        // A synthetic controller created for offline AdminCommand dispatch has
        // no live UNetConnection. Requiring the inherited Player property to
        // be an actual NetConnection prevents it (and CDOs) from ever becoming
        // a chat recipient.
        auto* player_property = find_property(controller->GetClassPrivate(), STR("Player"));
        if (!player_property || !player_property->IsA<RC::Unreal::FObjectProperty>())
        {
            return false;
        }

        const auto* player = player_property->ContainerPtrToValuePtr<UObject*>(controller);
        return player && *player && (*player)->GetClassPrivate() &&
               class_is_child_of((*player)->GetClassPrivate(), net_connection_base);
    }

    auto call_get_user_id(UObject* controller, UFunction* function, std::string& steam_id) -> bool
    {
        if (!controller || !function || function->GetReturnValueOffset() == k_no_return_value)
        {
            return false;
        }

        auto* return_property = function->GetReturnProperty();
        if (!return_property || !return_property->IsA<RC::Unreal::FStrProperty>())
        {
            return false;
        }

        FunctionParameterBuffer parameters{function};
        if (!parameters.valid())
        {
            return false;
        }
        controller->ProcessEvent(function, parameters.data());
        const auto result = parameters.read_string(return_property);
        if (!result || !is_steam_id64(*result))
        {
            return false;
        }
        steam_id = *result;
        return true;
    }
}

namespace simple_rcon
{
    struct RealChatDispatcher::RuntimeState
    {
        UClass* controller_base{};
        UClass* net_connection_base{};
        UFunction* get_user_id{};
        UObject* misc_statics{};
        UFunction* send_chat_line{};
        std::string detail{"not resolved yet"};
    };

    RealChatDispatcher::RealChatDispatcher() : m_state(new RuntimeState{}) {}

    RealChatDispatcher::~RealChatDispatcher()
    {
        delete m_state;
    }

    namespace
    {
        auto resolve_runtime(RealChatDispatcher::RuntimeState& state, std::string& detail) -> bool
        {
            if (!state.controller_base)
            {
                state.controller_base = find_object<UClass>(STR("/Script/SCUM.ConZPlayerController"));
                if (!state.controller_base)
                {
                    state.controller_base = find_object<UClass>(STR("/Script/ConZ.ConZPlayerController"));
                }
            }
            if (!state.net_connection_base)
            {
                state.net_connection_base = find_object<UClass>(STR("/Script/Engine.NetConnection"));
            }
            if (!state.get_user_id)
            {
                state.get_user_id = find_object<UFunction>(STR("/Script/SCUM.ConZPlayerController:GetUserId"));
                if (!state.get_user_id)
                {
                    state.get_user_id = find_object<UFunction>(STR("/Script/ConZ.ConZPlayerController:GetUserId"));
                }
            }
            if (!state.misc_statics)
            {
                state.misc_statics = find_object<UObject>(STR("/Script/SCUM.Default__MiscStatics"));
            }
            if (!state.send_chat_line)
            {
                state.send_chat_line = find_object<UFunction>(STR("/Script/SCUM.MiscStatics:SendChatLineToPlayer"));
            }

            if (!state.controller_base || !state.net_connection_base || !state.get_user_id || !state.misc_statics || !state.send_chat_line)
            {
                std::ostringstream output;
                output << "chat runtime unavailable:";
                if (!state.controller_base) output << " ConZPlayerController";
                if (!state.net_connection_base) output << " NetConnection";
                if (!state.get_user_id) output << " GetUserId";
                if (!state.misc_statics) output << " Default__MiscStatics";
                if (!state.send_chat_line) output << " SendChatLineToPlayer";
                state.detail = output.str();
                detail = state.detail;
                return false;
            }

            ChatSchema schema{};
            if (!discover_chat_schema(state.send_chat_line, schema))
            {
                state.detail = schema.detail;
                detail = state.detail;
                return false;
            }

            state.detail = "real controller lookup + " + schema.detail;
            detail = state.detail;
            return true;
        }

        auto send_chat_line(RealChatDispatcher::RuntimeState& state,
                            UObject* target_controller,
                            int type,
                            std::string_view message,
                            std::string& detail) -> bool
        {
            ChatSchema schema{};
            if (!discover_chat_schema(state.send_chat_line, schema))
            {
                detail = schema.detail;
                return false;
            }

            FunctionParameterBuffer parameters{state.send_chat_line};
            if (!parameters.valid() ||
                !parameters.write_object(schema.target_controller, target_controller) ||
                (schema.world_context && !parameters.write_object(schema.world_context, target_controller)) ||
                !parameters.write_string(schema.message, message) ||
                !parameters.write_small_integer(schema.message_type, type))
            {
                detail = "could not populate SendChatLineToPlayer parameters; " + schema.detail;
                return false;
            }

            state.misc_statics->ProcessEvent(state.send_chat_line, parameters.data());
            detail = schema.detail;
            return true;
        }

        auto find_live_controller_by_steam_id(RealChatDispatcher::RuntimeState& state,
                                              std::string_view steam_id) -> UObject*
        {
            UObject* result{};
            RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (result || !object || !object->GetClassPrivate() ||
                    !class_is_child_of(object->GetClassPrivate(), state.controller_base) ||
                    !controller_has_live_network_player(object, state.net_connection_base))
                {
                    return RC::LoopAction::Continue;
                }

                std::string candidate_id;
                if (call_get_user_id(object, state.get_user_id, candidate_id) && candidate_id == steam_id)
                {
                    result = object;
                    return RC::LoopAction::Break;
                }
                return RC::LoopAction::Continue;
            });
            return result;
        }

        auto enumerate_live_controllers(RealChatDispatcher::RuntimeState& state) -> std::vector<UObject*>
        {
            std::vector<UObject*> result;
            RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (!object || !object->GetClassPrivate() ||
                    !class_is_child_of(object->GetClassPrivate(), state.controller_base) ||
                    !controller_has_live_network_player(object, state.net_connection_base))
                {
                    return RC::LoopAction::Continue;
                }

                std::string steam_id;
                if (call_get_user_id(object, state.get_user_id, steam_id))
                {
                    result.emplace_back(object);
                }
                return RC::LoopAction::Continue;
            });
            return result;
        }
    }

    RealChatDispatcher::Result RealChatDispatcher::dispatch_if_chat(std::string_view command)
    {
        const auto tokens = tokenize(command);
        if (tokens.empty() || lowercase(tokens.front()) != "sendchat")
        {
            return {};
        }

        Result result{};
        result.handled = true;
        if (tokens.size() < 3 || tokens.size() > 4)
        {
            result.message = "usage: SendChat <type 0-7> \"message\" [target SteamID64]";
            return result;
        }

        int type{};
        const auto [end, error] = std::from_chars(tokens[1].data(), tokens[1].data() + tokens[1].size(), type);
        if (error != std::errc{} || end != tokens[1].data() + tokens[1].size() || type < 0 || type > 7)
        {
            result.message = "SendChat type must be an integer from 0 through 7";
            return result;
        }
        if (tokens[2].empty() || tokens[2].size() > k_max_message_bytes)
        {
            result.message = "SendChat message must contain 1-1024 UTF-8 bytes";
            return result;
        }

        std::string target_steam_id;
        if (tokens.size() == 4)
        {
            target_steam_id = tokens[3];
            if (!is_steam_id64(target_steam_id))
            {
                result.message = "SendChat target must be a 17-digit SteamID64";
                return result;
            }
        }

        if (!m_state)
        {
            result.message = "chat dispatcher has no runtime state";
            return result;
        }

        std::string detail;
        if (!resolve_runtime(*m_state, detail))
        {
            result.message = detail;
            return result;
        }

        if (!target_steam_id.empty())
        {
            auto* target = find_live_controller_by_steam_id(*m_state, target_steam_id);
            if (!target)
            {
                result.message = "SendChat target " + target_steam_id + " is not online with a real network controller";
                return result;
            }
            if (!send_chat_line(*m_state, target, type, tokens[2], detail))
            {
                result.message = detail;
                return result;
            }

            result.success = true;
            result.message = "ok: SendChat delivered to online SteamID " + target_steam_id;
            return result;
        }

        const auto targets = enumerate_live_controllers(*m_state);
        if (targets.empty())
        {
            result.message = "SendChat broadcast has no real online player controllers";
            return result;
        }

        int sent{};
        for (auto* target : targets)
        {
            if (!send_chat_line(*m_state, target, type, tokens[2], detail))
            {
                result.message = detail;
                return result;
            }
            ++sent;
        }

        result.success = true;
        result.message = "ok: SendChat broadcast delivered to " + std::to_string(sent) + " real online player(s)";
        return result;
    }

    RealChatDispatcher::Result RealChatDispatcher::probe()
    {
        Result result{};
        result.handled = true;
        if (!m_state)
        {
            result.message = "chat dispatcher has no runtime state";
            return result;
        }

        std::string detail;
        if (!resolve_runtime(*m_state, detail))
        {
            result.message = detail;
            return result;
        }

        const auto online = enumerate_live_controllers(*m_state);
        result.success = true;
        result.message = "ok: real chat dispatcher ready; online_real_controllers=" + std::to_string(online.size()) + "; " + detail;
        return result;
    }

    std::string RealChatDispatcher::status() const
    {
        return m_state ? "real_chat=" + m_state->detail : "real_chat=no runtime state";
    }
}
