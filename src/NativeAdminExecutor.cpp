#include "NativeAdminExecutor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

#include <UnrealDef.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace
{
    using RC::Unreal::FBoolProperty;
    using RC::Unreal::FArrayProperty;
    using RC::Unreal::FClassProperty;
    using RC::Unreal::FImplementedInterface;
    using RC::Unreal::FIntProperty;
    using RC::Unreal::FProperty;
    using RC::Unreal::FScriptArrayHelper;
    using RC::Unreal::FString;
    using RC::Unreal::FStrProperty;
    using RC::Unreal::UClass;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;

    // Recovered from the reference SCUM-RCON build and checked at runtime. A
    // build update must still produce exactly one match; no hardcoded RVA is
    // used and no guessed address is called.
    constexpr std::array<uint8_t, 24> k_executor_signature{
        0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
        0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x0F, 0x29,
        0x74, 0x24, 0x20, 0x48, 0x8D, 0x4C, 0x24, 0x40,
    };
    constexpr ptrdiff_t k_executor_entry_adjustment = -4;
    constexpr size_t k_max_vtable_bytes = 0x1000;
    constexpr size_t k_max_arguments = 64;

    auto lowercase(std::string value) -> std::string
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    auto normalize_verb(std::string value) -> std::string
    {
        value = lowercase(std::move(value));
        if (!value.empty() && value.front() == '#')
        {
            value.erase(value.begin());
        }
        return value;
    }

    auto is_steam_id64(std::string_view value) -> bool
    {
        return value.size() == 17 &&
               std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    auto fstring_to_utf8(const FString& value) -> std::string
    {
        const auto length = value.Len();
        if (length <= 0 || length > 4096)
        {
            return {};
        }
        const auto* characters = *value;
        if (!characters)
        {
            return {};
        }
        const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, characters, length, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
        {
            return {};
        }
        std::string result(static_cast<size_t>(bytes), '\0');
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, characters, length, result.data(), bytes, nullptr, nullptr);
        return result;
    }

    auto widen_utf8(std::string_view input, std::wstring& result) -> bool
    {
        result.clear();
        if (input.empty())
        {
            return true;
        }
        const int characters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
        if (characters <= 0)
        {
            return false;
        }
        result.resize(static_cast<size_t>(characters));
        return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), characters) == characters;
    }

    auto tokenize(std::string_view command, std::vector<std::string>& result) -> bool
    {
        std::string current;
        bool quoted = false;
        bool escaped = false;
        bool started = false;
        for (char c : command)
        {
            if (escaped)
            {
                current.push_back(c);
                escaped = false;
                started = true;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                started = true;
                continue;
            }
            if (c == '"')
            {
                quoted = !quoted;
                started = true;
                continue;
            }
            if (!quoted && std::isspace(static_cast<unsigned char>(c)))
            {
                if (started)
                {
                    result.emplace_back(std::move(current));
                    current.clear();
                    started = false;
                }
                continue;
            }
            current.push_back(c);
            started = true;
        }
        if (quoted)
        {
            return false;
        }
        if (escaped)
        {
            current.push_back('\\');
        }
        if (started)
        {
            result.emplace_back(std::move(current));
        }
        return true;
    }

    template <typename T>
    auto find_object(const wchar_t* path) -> T*
    {
        return RC::Unreal::UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
    }

    auto class_is_child_of(UClass* candidate, UClass* expected_base) -> bool
    {
        for (auto* current = candidate; current; current = current->GetSuperClass())
        {
            if (current == expected_base)
            {
                return true;
            }
        }
        return false;
    }

    auto valid(UObject* object) -> bool
    {
        if (!object)
        {
            return false;
        }
        const auto index = object->GetInternalIndex();
        if (index < 0 || index >= RC::Unreal::FUObjectArray::GetNumElements())
        {
            return false;
        }
        auto* item = RC::Unreal::FUObjectArray::IndexToObject(index);
        return item && item->GetUObject() == object && item->IsValid(false);
    }

    auto live(UObject* object) -> bool
    {
        return valid(object) &&
               !(object->GetObjectFlags() & (RC::Unreal::RF_ClassDefaultObject | RC::Unreal::RF_ArchetypeObject));
    }

    auto safe_read(const void* source, void* target, size_t bytes) -> bool
    {
        SIZE_T completed{};
        return source && target && bytes > 0 &&
               ::ReadProcessMemory(::GetCurrentProcess(), source, target, bytes, &completed) &&
               completed == bytes;
    }

    auto object_property(UObject* object, const wchar_t* name) -> UObject*
    {
        if (!object)
        {
            return nullptr;
        }
        auto* property = RC::Unreal::CastField<RC::Unreal::FObjectPropertyBase>(object->GetPropertyByNameInChain(name));
        return property ? property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object)) : nullptr;
    }

    auto implements_interface(UObject* object, UClass* interface_class) -> bool
    {
        if (!object || !interface_class || !object->GetClassPrivate())
        {
            return false;
        }
        try
        {
            for (auto* current = object->GetClassPrivate(); current; current = current->GetSuperClass())
            {
                for (const FImplementedInterface& item : current->GetInterfaces())
                {
                    if (item.Class == interface_class)
                    {
                        return true;
                    }
                }
            }
        }
        catch (...)
        {
            return false;
        }
        return false;
    }

    auto scan_main_module_for_executor(std::string& detail) -> void*
    {
        const auto module = ::GetModuleHandleW(nullptr);
        if (!module)
        {
            detail = "GetModuleHandleW(nullptr) failed";
            return nullptr;
        }
        const auto* image = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            detail = "main module has no DOS header";
            return nullptr;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            detail = "main module has no NT header";
            return nullptr;
        }
        const IMAGE_SECTION_HEADER* text = nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if (std::memcmp(sections[i].Name, ".text", 5) == 0)
            {
                text = &sections[i];
                break;
            }
        }
        if (!text || text->Misc.VirtualSize < k_executor_signature.size())
        {
            detail = "main module .text section is unavailable";
            return nullptr;
        }
        const auto* begin = image + text->VirtualAddress;
        const auto length = static_cast<size_t>(text->Misc.VirtualSize);
        const uint8_t* match = nullptr;
        size_t matches = 0;
        for (size_t offset = 0; offset + k_executor_signature.size() <= length; ++offset)
        {
            if (std::memcmp(begin + offset, k_executor_signature.data(), k_executor_signature.size()) == 0)
            {
                match = begin + offset;
                ++matches;
            }
        }
        if (matches != 1)
        {
            detail = matches == 0 ? "native AdminCommand executor signature not found" :
                                    "native AdminCommand executor signature is ambiguous (" + std::to_string(matches) + " matches)";
            return nullptr;
        }
        detail = "native AdminCommand executor resolved by one exact code signature";
        return const_cast<uint8_t*>(match + k_executor_entry_adjustment);
    }

    auto vtable_slot(UObject* object, size_t offset) -> uintptr_t
    {
        uintptr_t table{};
        uintptr_t function{};
        if (!safe_read(object, &table, sizeof(table)) || !table ||
            !safe_read(reinterpret_cast<void*>(table + offset), &function, sizeof(function)))
        {
            return 0;
        }
        return function;
    }

    auto find_executor_slot(UObject* object, uintptr_t executor, size_t& slot, std::string& detail) -> bool
    {
        slot = 0;
        size_t matches = 0;
        for (size_t offset = 0; offset < k_max_vtable_bytes; offset += sizeof(void*))
        {
            if (vtable_slot(object, offset) == executor)
            {
                slot = offset;
                ++matches;
            }
        }
        if (matches != 1)
        {
            detail = matches == 0 ? "executor is absent from the command vtable" :
                                    "executor is ambiguous in the command vtable";
            return false;
        }
        detail = "executor vtable slot=0x" + [&] {
            std::ostringstream output;
            output << std::hex << slot;
            return output.str();
        }();
        return true;
    }

    auto function_is_executable(uintptr_t address) -> bool
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !::VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)))
        {
            return false;
        }
        const auto protect = info.Protect & 0xff;
        return info.AllocationBase == ::GetModuleHandleW(nullptr) && info.State == MEM_COMMIT &&
               !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
               (protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
                protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
    }

    auto get_user_id(UObject* controller, UFunction* function, std::string& steam_id) -> bool
    {
        if (!live(controller) || !function || function->GetParmsSize() != sizeof(FString))
        {
            return false;
        }
        auto* result_property = function->GetReturnProperty();
        if (!result_property || !result_property->IsA<FStrProperty>())
        {
            return false;
        }
        FString value{};
        controller->ProcessEvent(function, &value);
        const auto text = fstring_to_utf8(value);
        if (!is_steam_id64(text))
        {
            return false;
        }
        steam_id = text;
        return true;
    }
}

namespace simple_rcon
{
    struct NativeAdminExecutor::RuntimeState
    {
        UClass* admin_command_base{};
        UClass* controller_base{};
        UClass* net_connection_base{};
        UClass* prisoner_class{};
        UClass* executor_interface{};
        UFunction* get_user_id{};
        UObject* registry{};
        uintptr_t native_executor{};
        size_t executor_slot{};
        std::unordered_map<std::string, UObject*> verbs;
        std::string registry_detail{"not resolved yet"};
        std::string executor_detail{"not resolved yet"};
        std::string context_detail{"no command executed yet"};
        bool native_faulted{};
    };

    NativeAdminExecutor::NativeAdminExecutor() : m_state(new RuntimeState{}) {}

    NativeAdminExecutor::~NativeAdminExecutor()
    {
        delete m_state;
    }

    void NativeAdminExecutor::configure(bool enabled, bool native_executor_enabled)
    {
        m_enabled = enabled;
        m_native_executor_enabled = native_executor_enabled;
    }

    void NativeAdminExecutor::on_unreal_init()
    {
        m_unreal_ready = true;
    }

    namespace
    {
        auto resolve_runtime(NativeAdminExecutor::RuntimeState& state, std::string& detail) -> bool
        {
            if (!state.admin_command_base) state.admin_command_base = find_object<UClass>(STR("/Script/SCUM.AdminCommand"));
            if (!state.controller_base) state.controller_base = find_object<UClass>(STR("/Script/SCUM.ConZPlayerController"));
            if (!state.net_connection_base) state.net_connection_base = find_object<UClass>(STR("/Script/Engine.NetConnection"));
            if (!state.prisoner_class) state.prisoner_class = find_object<UClass>(STR("/Script/SCUM.Prisoner"));
            if (!state.executor_interface) state.executor_interface = find_object<UClass>(STR("/Script/SCUM.AdminCommandExecutor"));
            if (!state.get_user_id) state.get_user_id = find_object<UFunction>(STR("/Script/SCUM.ConZPlayerController:GetUserId"));
            if (!state.admin_command_base || !state.controller_base || !state.net_connection_base ||
                !state.prisoner_class || !state.executor_interface || !state.get_user_id)
            {
                std::ostringstream output;
                output << "native command runtime unavailable:";
                if (!state.admin_command_base) output << " AdminCommand";
                if (!state.controller_base) output << " ConZPlayerController";
                if (!state.net_connection_base) output << " NetConnection";
                if (!state.prisoner_class) output << " Prisoner";
                if (!state.executor_interface) output << " AdminCommandExecutor";
                if (!state.get_user_id) output << " GetUserId";
                detail = output.str();
                return false;
            }
            return true;
        }

        auto build_registry(NativeAdminExecutor::RuntimeState& state, std::string& detail) -> bool
        {
            auto* registry_class = find_object<UClass>(STR("/Script/SCUM.AdminCommandRegistry"));
            if (!registry_class || !state.admin_command_base)
            {
                detail = "AdminCommandRegistry reflection is not loaded yet";
                return false;
            }

            UObject* registry{};
            unsigned registry_matches{};
            RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (live(object) && object->IsA(registry_class))
                {
                    registry = object;
                    ++registry_matches;
                }
                return RC::LoopAction::Continue;
            });
            if (registry_matches != 1)
            {
                detail = registry_matches ? "multiple live AdminCommandRegistry objects" : "AdminCommandRegistry is not ready";
                return false;
            }

            auto* commands = RC::Unreal::CastField<FArrayProperty>(registry->GetPropertyByNameInChain(STR("_commands")));
            auto* class_inner = commands ? RC::Unreal::CastField<FClassProperty>(commands->GetInner()) : nullptr;
            if (!commands || !class_inner || class_inner->GetElementSize() != sizeof(void*))
            {
                detail = "AdminCommandRegistry._commands is not a class array";
                return false;
            }

            FScriptArrayHelper values(commands, commands->ContainerPtrToValuePtr<void>(registry));
            const auto count = values.Num();
            if (count <= 0 || count > 4096)
            {
                detail = "AdminCommandRegistry command count is invalid";
                return false;
            }

            std::unordered_map<std::string, UObject*> next;
            for (int32_t i = 0; i < count; ++i)
            {
                auto* command_class = static_cast<UClass*>(class_inner->GetObjectPropertyValue(values.GetRawPtr(i)));
                if (!valid(command_class) || !class_is_child_of(command_class, state.admin_command_base))
                {
                    continue;
                }
                auto* cdo = command_class->GetClassDefaultObject().Get();
                if (!valid(cdo) || !(cdo->GetObjectFlags() & RC::Unreal::RF_ClassDefaultObject))
                {
                    continue;
                }
                auto* verb_property = RC::Unreal::CastField<FStrProperty>(cdo->GetPropertyByNameInChain(STR("_verb")));
                if (!verb_property)
                {
                    continue;
                }
                auto verb = normalize_verb(fstring_to_utf8(verb_property->GetPropertyValueInContainer(cdo)));
                if (verb.empty())
                {
                    continue;
                }
                if (!next.try_emplace(verb, cdo).second)
                {
                    detail = "AdminCommandRegistry contains duplicate verb '" + verb + "'";
                    return false;
                }
            }
            if (next.empty())
            {
                detail = "AdminCommandRegistry contains no usable AdminCommand classes";
                return false;
            }

            state.registry = registry;
            state.verbs = std::move(next);
            state.registry_detail = "registry commands=" + std::to_string(state.verbs.size());
            detail = state.registry_detail;
            return true;
        }

        auto resolve_executor(NativeAdminExecutor::RuntimeState& state, std::string& detail) -> bool
        {
            if (state.native_faulted)
            {
                detail = "native execution is disabled after an exception; restart required";
                return false;
            }
            if (!state.native_executor)
            {
                state.native_executor = reinterpret_cast<uintptr_t>(scan_main_module_for_executor(state.executor_detail));
            }
            if (!state.native_executor || !function_is_executable(state.native_executor))
            {
                detail = state.executor_detail;
                return false;
            }

            // Derive the virtual slot from the live command CDO. This avoids
            // treating a reference-build vtable offset as a current-build RVA.
            size_t verified{};
            size_t common_slot{std::numeric_limits<size_t>::max()};
            UObject* verified_cdo{};
            std::string last_detail;
            for (const auto& [verb, cdo] : state.verbs)
            {
                size_t slot{};
                std::string command_detail;
                if (!find_executor_slot(cdo, state.native_executor, slot, command_detail))
                {
                    last_detail = command_detail;
                    continue;
                }
                if (common_slot == std::numeric_limits<size_t>::max())
                {
                    common_slot = slot;
                }
                else if (slot != common_slot)
                {
                    detail = "AdminCommand executor appears in inconsistent vtable slots";
                    return false;
                }
                verified_cdo = cdo;
                ++verified;
            }
            if (!verified || !verified_cdo || common_slot == std::numeric_limits<size_t>::max())
            {
                detail = "native executor is executable but not present in a live AdminCommand vtable";
                if (!last_detail.empty())
                {
                    detail += ": " + last_detail;
                }
                return false;
            }
            state.executor_slot = common_slot;
            state.executor_detail = "executor verified in " + std::to_string(verified) +
                                    " command CDO vtables at slot=0x" + [&] {
                std::ostringstream output;
                output << std::hex << state.executor_slot;
                return output.str();
            }();
            detail = state.executor_detail;
            return true;
        }

        auto find_controller(NativeAdminExecutor::RuntimeState& state, std::string_view steam_id, std::string& detail) -> UObject*
        {
            UObject* result{};
            size_t matches{};
            RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (!live(object) || !object->GetClassPrivate() ||
                    !class_is_child_of(object->GetClassPrivate(), state.controller_base))
                {
                    return RC::LoopAction::Continue;
                }
                auto* connection = object_property(object, STR("NetConnection"));
                if (!live(connection) || !connection->GetClassPrivate() ||
                    !class_is_child_of(connection->GetClassPrivate(), state.net_connection_base))
                {
                    return RC::LoopAction::Continue;
                }
                std::string candidate_id;
                if (get_user_id(object, state.get_user_id, candidate_id) && candidate_id == steam_id)
                {
                    result = object;
                    ++matches;
                }
                return RC::LoopAction::Continue;
            });
            if (matches != 1)
            {
                detail = matches == 0 ? "target SteamID is not connected with a live network controller" :
                                        "target SteamID matched multiple live controllers";
                return nullptr;
            }
            return result;
        }

        auto call_native(uintptr_t function, UObject* instance, RC::Unreal::TArray<FString>* arguments,
                         bool& faulted) -> bool
        {
            using Execute = bool(__fastcall*)(UObject*, RC::Unreal::TArray<FString>*);
            const auto execute = reinterpret_cast<Execute>(function);
#if defined(_MSC_VER)
            __try
            {
                return execute(instance, arguments);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                faulted = true;
                return false;
            }
#else
            return execute(instance, arguments);
#endif
        }

        auto checked_flag(UObject* cdo, const wchar_t* name, bool& value) -> bool
        {
            auto* property = RC::Unreal::CastField<FBoolProperty>(cdo->GetPropertyByNameInChain(name));
            if (!property)
            {
                return false;
            }
            value = property->GetPropertyValueInContainer(cdo);
            return true;
        }
    }

    NativeAdminExecutor::Result NativeAdminExecutor::execute(std::string_view command)
    {
        std::vector<std::string> tokens;
        if (!tokenize(command, tokens))
        {
            return {false, false, "native command has an unterminated quoted argument"};
        }
        if (tokens.empty() || lowercase(tokens.front()) != "!exec")
        {
            return {};
        }

        Result result{true, false, {}};
        if (!m_enabled)
        {
            result.message = "native AdminCommand dispatcher is disabled";
            return result;
        }
        if (!m_unreal_ready || !m_state)
        {
            result.message = "native AdminCommand dispatcher is not ready";
            return result;
        }
        if (tokens.size() < 3 || tokens.size() > k_max_arguments + 3)
        {
            result.message = "usage: !exec <SteamID64> <command> [arguments]";
            return result;
        }
        if (!is_steam_id64(tokens[1]))
        {
            result.message = "execution context must be a 17-digit SteamID64";
            return result;
        }
        if (m_state->native_faulted)
        {
            result.message = "native execution is disabled after an exception; restart required";
            return result;
        }

        std::string detail;
        if (!resolve_runtime(*m_state, detail))
        {
            result.message = detail;
            return result;
        }
        if (!build_registry(*m_state, detail))
        {
            result.message = "native AdminCommand registry unavailable: " + detail;
            return result;
        }
        if (!m_native_executor_enabled)
        {
            result.message = "native AdminCommand executor is disabled by config";
            return result;
        }
        if (!resolve_executor(*m_state, detail))
        {
            result.message = "native AdminCommand executor unavailable: " + detail;
            return result;
        }

        const auto verb = normalize_verb(tokens[2]);
        const auto entry = m_state->verbs.find(verb);
        if (entry == m_state->verbs.end())
        {
            result.message = "command not found in SCUM AdminCommandRegistry: " + tokens[2];
            return result;
        }
        auto* cdo = entry->second;
        auto* command_class = cdo->GetClassPrivate();

        bool enabled{};
        bool shipping{};
        bool server{};
        auto* required_property = RC::Unreal::CastField<FIntProperty>(cdo->GetPropertyByNameInChain(STR("_numberOfRequiredArguments")));
        auto* repeating_property = RC::Unreal::CastField<FIntProperty>(cdo->GetPropertyByNameInChain(STR("_numberOfRepeatingArguments")));
        auto* arguments_property = RC::Unreal::CastField<FArrayProperty>(cdo->GetPropertyByNameInChain(STR("_argumentDescriptions")));
        if (!required_property || !repeating_property || !arguments_property || !arguments_property->GetInner() ||
            !checked_flag(cdo, STR("_isEnabled"), enabled) ||
            !checked_flag(cdo, STR("_isEnabledInShippingBuild"), shipping) ||
            !checked_flag(cdo, STR("_shouldExecuteOnServer"), server))
        {
            result.message = "SCUM AdminCommand metadata is incompatible for '" + tokens[2] + "'";
            return result;
        }
        const int required = required_property->GetPropertyValueInContainer(cdo);
        const int repeating = repeating_property->GetPropertyValueInContainer(cdo);
        FScriptArrayHelper descriptions(arguments_property, arguments_property->ContainerPtrToValuePtr<void>(cdo));
        const int declared = descriptions.Num();
        const int count = static_cast<int>(tokens.size() - 3);
        if (!enabled || !shipping || !server)
        {
            result.message = "SCUM AdminCommand is disabled or client-only: '" + tokens[2] + "'";
            return result;
        }
        if (required < 0 || declared < 0 || required > declared || declared > 64 || repeating < 0 || repeating > declared)
        {
            result.message = "SCUM AdminCommand argument metadata is incompatible for '" + tokens[2] + "'";
            return result;
        }
        if (count < required || (!repeating && count > declared) ||
            (repeating && count > declared && (count - declared) % repeating != 0))
        {
            result.message = "argument count mismatch for '" + tokens[2] + "': required=" + std::to_string(required) +
                             " declared=" + std::to_string(declared) + " repeating=" + std::to_string(repeating);
            return result;
        }

        const auto controller = find_controller(*m_state, tokens[1], detail);
        if (!controller)
        {
            result.message = detail;
            return result;
        }
        auto* pawn = object_property(controller, STR("Pawn"));
        if (!live(pawn) || !pawn->GetClassPrivate() || !class_is_child_of(pawn->GetClassPrivate(), m_state->prisoner_class))
        {
            result.message = "target SteamID has no live Prisoner pawn";
            return result;
        }
        if (!implements_interface(controller, m_state->executor_interface))
        {
            result.message = "target controller does not implement SCUM AdminCommandExecutor";
            return result;
        }

        size_t command_slot{};
        std::string slot_detail;
        if (!find_executor_slot(cdo, m_state->native_executor, command_slot, slot_detail) ||
            command_slot != m_state->executor_slot)
        {
            result.message = "SCUM AdminCommand CDO executor vtable changed before execution: " + slot_detail;
            return result;
        }

        RC::Unreal::FStaticConstructObjectParameters construction{command_class, controller};
        auto* instance = RC::Unreal::UObjectGlobals::StaticConstructObject(construction);
        if (!live(instance) || instance->GetClassPrivate() != command_class || instance->GetOuterPrivate() != controller)
        {
            result.message = "could not construct AdminCommand with the real controller as Outer";
            return result;
        }
        size_t instance_slot{};
        if (!find_executor_slot(instance, m_state->native_executor, instance_slot, slot_detail) ||
            instance_slot != command_slot)
        {
            result.message = "native AdminCommand instance vtable validation failed: " + slot_detail;
            return result;
        }

        RC::Unreal::TArray<FString> arguments{};
        for (size_t i = 3; i < tokens.size(); ++i)
        {
            std::wstring value;
            if (!widen_utf8(tokens[i], value))
            {
                result.message = "native command arguments must use valid UTF-8";
                return result;
            }
            arguments.Add(FString{value.c_str()});
        }
        bool faulted{};
        const bool executed = call_native(m_state->native_executor, instance, &arguments, faulted);
        if (faulted)
        {
            m_state->native_faulted = true;
            result.message = "SCUM AdminCommand raised a native exception; execution status is unknown and further native dispatch is disabled until restart";
            return result;
        }
        m_state->context_detail = "steam=" + tokens[1] + "; prisoner=live; slot=0x" + [&] {
            std::ostringstream output;
            output << std::hex << command_slot;
            return output.str();
        }();
        if (!executed)
        {
            result.message = "SCUM AdminCommand returned false for '" + tokens[2] + "'";
            return result;
        }
        result.success = true;
        result.message = "ok: native AdminCommand '" + tokens[2] + "' executed for " + tokens[1];
        return result;
    }

    NativeAdminExecutor::Result NativeAdminExecutor::probe()
    {
        Result result{true, false, {}};
        if (!m_enabled)
        {
            result.message = "native AdminCommand dispatcher is disabled";
            return result;
        }
        if (!m_unreal_ready || !m_state)
        {
            result.message = "native AdminCommand dispatcher is not ready";
            return result;
        }
        std::string detail;
        if (!resolve_runtime(*m_state, detail))
        {
            result.message = detail;
            return result;
        }
        if (!build_registry(*m_state, detail))
        {
            result.message = "native AdminCommand registry unavailable: " + detail;
            return result;
        }
        if (!m_native_executor_enabled)
        {
            result.message = "native AdminCommand executor is disabled by config";
            return result;
        }
        if (!resolve_executor(*m_state, detail))
        {
            result.message = "native AdminCommand executor unavailable: " + detail;
            return result;
        }
        result.success = true;
        result.message = "ok: native AdminCommand dispatcher ready; " + status();
        return result;
    }

    std::string NativeAdminExecutor::status() const
    {
        std::ostringstream output;
        output << "native_dispatch=" << (m_enabled ? "enabled" : "disabled")
               << "; native_executor=" << (m_native_executor_enabled ? "enabled" : "disabled");
        if (!m_state)
        {
            return output.str();
        }
        output << "; verbs=" << m_state->verbs.size()
               << "; registry=" << m_state->registry_detail
               << "; executor=" << m_state->executor_detail
               << "; context=" << m_state->context_detail;
        if (m_state->native_faulted)
        {
            output << "; latched=faulted-until-restart";
        }
        return output.str();
    }
}
