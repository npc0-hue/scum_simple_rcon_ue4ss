#include "OfflineAdminExecutor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <UnrealDef.hpp>

namespace
{
    using RC::Unreal::FProperty;
    using RC::Unreal::FString;
    using RC::Unreal::UClass;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;

    constexpr auto k_mark_as_root_set = static_cast<RC::Unreal::EObjectFlags>(0x80);
    constexpr size_t k_function_parameter_bytes = 0x200;

    // This is the exact native AdminCommand executor signature recovered from
    // the reference SCUM-RCON build.  It is intentionally not fuzzy/wildcard
    // matched: a game update must produce one exact hit or dispatch is refused.
    constexpr std::array<uint8_t, 24> k_executor_signature{
        0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
        0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x0F, 0x29,
        0x74, 0x24, 0x20, 0x48, 0x8D, 0x4C, 0x24, 0x40,
    };

    // The matched bytes begin four bytes after the native function entry.
    constexpr ptrdiff_t k_executor_entry_adjustment = -4;

    struct alignas(16) IdentityTransform
    {
        float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float translation[4]{0.0f, 0.0f, 0.0f, 0.0f};
        float scale[4]{1.0f, 1.0f, 1.0f, 0.0f};
    };

    static_assert(sizeof(IdentityTransform) == 0x30);

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

    auto lowercase(std::string value) -> std::string
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
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

    template <typename T>
    auto find_object(const wchar_t* path) -> T*
    {
        return RC::Unreal::UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
    }

    auto find_property(UFunction* function, const wchar_t* name) -> FProperty*
    {
        if (!function)
        {
            return nullptr;
        }

        for (FProperty* property : RC::Unreal::TFieldRange<FProperty>(function, RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
        {
            if (property && property->GetName() == name)
            {
                return property;
            }
        }
        return nullptr;
    }

    auto find_property(UClass* klass, const wchar_t* name) -> FProperty*
    {
        if (!klass)
        {
            return nullptr;
        }

        for (FProperty* property : RC::Unreal::TFieldRange<FProperty>(klass, RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
        {
            if (property && property->GetName() == name)
            {
                return property;
            }
        }
        return nullptr;
    }

    auto property_offset(UFunction* function, const wchar_t* name) -> int32_t
    {
        const auto* property = find_property(function, name);
        return property ? property->GetOffset_Internal() : -1;
    }

    template <typename T>
    auto write_parameter(std::array<uint8_t, k_function_parameter_bytes>& parameters, int32_t offset, const T& value) -> bool
    {
        if (offset < 0 || static_cast<size_t>(offset) + sizeof(T) > parameters.size())
        {
            return false;
        }
        std::memcpy(parameters.data() + offset, &value, sizeof(T));
        return true;
    }

    template <typename T>
    auto read_parameter(const std::array<uint8_t, k_function_parameter_bytes>& parameters, int32_t offset) -> std::optional<T>
    {
        if (offset < 0 || static_cast<size_t>(offset) + sizeof(T) > parameters.size())
        {
            return std::nullopt;
        }
        T value{};
        std::memcpy(&value, parameters.data() + offset, sizeof(T));
        return value;
    }

    auto call_function(UObject* target, UFunction* function, void* parameters) -> bool
    {
        if (!target || !function)
        {
            return false;
        }
        target->ProcessEvent(function, parameters);
        return true;
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

    auto looks_like_UClass(UObject* object) -> bool
    {
        if (!object || !object->GetClassPrivate())
        {
            return false;
        }
        const auto meta_name = lowercase(narrow(object->GetClassPrivate()->GetName()));
        return meta_name == "class" || meta_name == "blueprintgeneratedclass";
    }

    auto class_verb(UClass* klass) -> std::string
    {
        if (!klass)
        {
            return {};
        }
        std::string name = lowercase(narrow(klass->GetName()));
        constexpr std::string_view marker{"admincommand_"};
        const auto marker_pos = name.find(marker);
        if (marker_pos == std::string::npos)
        {
            return {};
        }
        std::string verb = name.substr(marker_pos + marker.size());
        if (verb.ends_with("_c"))
        {
            verb.resize(verb.size() - 2);
        }
        return verb;
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

        const IMAGE_SECTION_HEADER* text_section = nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (std::memcmp(section.Name, ".text", 5) == 0)
            {
                text_section = &section;
                break;
            }
        }
        if (!text_section || text_section->Misc.VirtualSize < k_executor_signature.size())
        {
            detail = "main module .text section is unavailable";
            return nullptr;
        }

        const auto* begin = image + text_section->VirtualAddress;
        const size_t length = text_section->Misc.VirtualSize;
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

        detail = "native AdminCommand executor resolved by exact signature";
        return const_cast<uint8_t*>(match + k_executor_entry_adjustment);
    }

    auto call_native_admin_executor(void* native_executor,
                                    UObject* command_instance,
                                    RC::Unreal::TArray<FString>* arguments,
                                    bool& faulted) -> int64_t
    {
        using NativeAdminExecutor = int64_t(__fastcall*)(UObject*, RC::Unreal::TArray<FString>*);
        const auto execute_native = reinterpret_cast<NativeAdminExecutor>(native_executor);
#if defined(_MSC_VER)
        __try
        {
            return execute_native(command_instance, arguments);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
            return 0;
        }
#else
        return execute_native(command_instance, arguments);
#endif
    }
}

namespace simple_rcon
{
    struct OfflineAdminExecutor::RuntimeState
    {
        UObject* world{};
        UObject* controller{};
        UObject* pawn{};
        UObject* user_profile{};
        UClass* admin_command_base{};
        std::unordered_map<std::string, UClass*> verbs;
        void* native_executor{};
        std::string bound_admin_steam_id;
        std::string native_executor_detail{"not resolved yet"};
        std::string last_context_detail{"not built yet"};
    };

    OfflineAdminExecutor::OfflineAdminExecutor() : m_state(new RuntimeState{}) {}

    OfflineAdminExecutor::~OfflineAdminExecutor()
    {
        delete m_state;
    }

    void OfflineAdminExecutor::configure(bool enabled, bool native_executor_enabled)
    {
        m_enabled = enabled;
        m_native_executor_enabled = native_executor_enabled;
    }

    void OfflineAdminExecutor::on_unreal_init()
    {
        m_unreal_ready = true;
    }

    namespace
    {
        auto make_identity_transform() -> IdentityTransform
        {
            return {};
        }

        auto spawn_actor(UObject* world, UClass* actor_class, std::string& detail) -> UObject*
        {
            auto* gameplay_statics = find_object<UObject>(STR("/Script/Engine.Default__GameplayStatics"));
            auto* begin_spawn = find_object<UFunction>(STR("/Script/Engine.GameplayStatics:BeginSpawningActorFromClass"));
            auto* finish_spawn = find_object<UFunction>(STR("/Script/Engine.GameplayStatics:FinishSpawningActor"));
            if (!world || !actor_class || !gameplay_statics || !begin_spawn || !finish_spawn)
            {
                detail = "GameplayStatics deferred-spawn objects are not available";
                return nullptr;
            }

            const int32_t world_offset = property_offset(begin_spawn, STR("WorldContextObject"));
            const int32_t class_offset = property_offset(begin_spawn, STR("ActorClass"));
            const int32_t transform_offset = property_offset(begin_spawn, STR("SpawnTransform"));
            const int32_t collision_fail_offset = property_offset(begin_spawn, STR("bNoCollisionFail"));
            const int32_t collision_mode_offset = property_offset(begin_spawn, STR("CollisionHandlingOverride"));
            const int32_t return_offset = property_offset(begin_spawn, STR("ReturnValue"));
            if (world_offset < 0 || class_offset < 0 || transform_offset < 0 || return_offset < 0)
            {
                detail = "BeginSpawningActorFromClass parameters are not compatible";
                return nullptr;
            }

            std::array<uint8_t, k_function_parameter_bytes> begin_parameters{};
            const IdentityTransform transform = make_identity_transform();
            const uint8_t true_byte = 1;
            const uint8_t always_spawn = 1;
            if (!write_parameter(begin_parameters, world_offset, world) ||
                !write_parameter(begin_parameters, class_offset, actor_class) ||
                !write_parameter(begin_parameters, transform_offset, transform) ||
                !write_parameter(begin_parameters, return_offset, static_cast<UObject*>(nullptr)) ||
                (collision_fail_offset >= 0 && !write_parameter(begin_parameters, collision_fail_offset, true_byte)) ||
                (collision_mode_offset >= 0 && !write_parameter(begin_parameters, collision_mode_offset, always_spawn)))
            {
                detail = "BeginSpawningActorFromClass parameter layout is too large";
                return nullptr;
            }
            if (!call_function(gameplay_statics, begin_spawn, begin_parameters.data()))
            {
                detail = "BeginSpawningActorFromClass could not be called";
                return nullptr;
            }

            const auto deferred_actor = read_parameter<UObject*>(begin_parameters, return_offset).value_or(nullptr);
            if (!deferred_actor)
            {
                detail = "BeginSpawningActorFromClass returned null";
                return nullptr;
            }
            deferred_actor->SetFlags(k_mark_as_root_set);

            const int32_t actor_offset = property_offset(finish_spawn, STR("Actor"));
            const int32_t finish_transform_offset = property_offset(finish_spawn, STR("SpawnTransform"));
            const int32_t finish_return_offset = property_offset(finish_spawn, STR("ReturnValue"));
            if (actor_offset < 0 || finish_transform_offset < 0 || finish_return_offset < 0)
            {
                detail = "FinishSpawningActor parameters are not compatible";
                return nullptr;
            }

            std::array<uint8_t, k_function_parameter_bytes> finish_parameters{};
            if (!write_parameter(finish_parameters, actor_offset, deferred_actor) ||
                !write_parameter(finish_parameters, finish_transform_offset, transform) ||
                !write_parameter(finish_parameters, finish_return_offset, static_cast<UObject*>(nullptr)))
            {
                detail = "FinishSpawningActor parameter layout is too large";
                return nullptr;
            }
            if (!call_function(gameplay_statics, finish_spawn, finish_parameters.data()))
            {
                detail = "FinishSpawningActor could not be called";
                return nullptr;
            }

            auto* actor = read_parameter<UObject*>(finish_parameters, finish_return_offset).value_or(deferred_actor);
            if (!actor)
            {
                detail = "FinishSpawningActor returned null";
                return nullptr;
            }
            actor->SetFlags(k_mark_as_root_set);
            return actor;
        }

        void disable_actor_tick(UObject* actor)
        {
            auto* function = find_object<UFunction>(STR("/Script/Engine.Actor:SetActorTickEnabled"));
            if (!actor || !function)
            {
                return;
            }
            const int32_t enabled_offset = property_offset(function, STR("bEnabled"));
            if (enabled_offset < 0)
            {
                return;
            }
            std::array<uint8_t, k_function_parameter_bytes> parameters{};
            const uint8_t false_byte = 0;
            if (write_parameter(parameters, enabled_offset, false_byte))
            {
                call_function(actor, function, parameters.data());
            }
        }

        void best_effort_bind_admin_identity(UObject* object, std::string_view steam_id)
        {
            if (!object || steam_id.empty() || !object->GetClassPrivate())
            {
                return;
            }

            const std::wstring steam_id_wide = widen_utf8(steam_id);
            uint64_t numeric_id{};
            try
            {
                numeric_id = std::stoull(std::string{steam_id});
            }
            catch (...)
            {
                return;
            }

            for (FProperty* property : RC::Unreal::TFieldRange<FProperty>(object->GetClassPrivate(), RC::Unreal::EFieldIterationFlags::IncludeDeprecated))
            {
                if (!property)
                {
                    continue;
                }
                const std::string property_name = lowercase(narrow(property->GetName()));
                const bool looks_like_identity = property_name.find("steam") != std::string::npos ||
                                                 property_name == "userid" || property_name == "_userid" ||
                                                 property_name.find("onlineid") != std::string::npos;
                if (!looks_like_identity)
                {
                    continue;
                }

                if (property->IsA<RC::Unreal::FStrProperty>())
                {
                    auto* value = property->ContainerPtrToValuePtr<FString>(object);
                    if (value)
                    {
                        *value = FString{steam_id_wide.c_str()};
                    }
                }
                else if (property->IsA<RC::Unreal::FUInt64Property>())
                {
                    auto* value = property->ContainerPtrToValuePtr<uint64_t>(object);
                    if (value)
                    {
                        *value = numeric_id;
                    }
                }
                else if (property->IsA<RC::Unreal::FInt64Property>())
                {
                    auto* value = property->ContainerPtrToValuePtr<int64_t>(object);
                    if (value)
                    {
                        *value = static_cast<int64_t>(numeric_id);
                    }
                }
            }
        }
    }

    namespace
    {
        auto ensure_synthetic_caller(OfflineAdminExecutor::RuntimeState& state,
                                     std::string_view configured_admin_steam_id,
                                     std::string& detail) -> bool
        {
            auto* current_world = RC::Unreal::UObjectGlobals::FindFirstOf(STR("World"));
            if (!current_world)
            {
                detail = "no UWorld is available yet";
                return false;
            }

            if (state.world != current_world)
            {
                state.world = current_world;
                state.controller = nullptr;
                state.pawn = nullptr;
                state.user_profile = nullptr;
                state.bound_admin_steam_id.clear();
                state.last_context_detail = "world changed; synthetic caller will be rebuilt";
            }

            if (state.controller && state.pawn)
            {
                // AdminUsers.ini is re-read by ScumBridge. Do not leave an
                // already-created synthetic caller bound to a SteamID that was
                // removed from the file or superseded through preferred_admin_steam_id.
                if (state.bound_admin_steam_id != configured_admin_steam_id)
                {
                    best_effort_bind_admin_identity(state.user_profile, configured_admin_steam_id);
                    best_effort_bind_admin_identity(state.controller, configured_admin_steam_id);
                    state.bound_admin_steam_id = std::string{configured_admin_steam_id};
                    detail = "reusing synthetic controller and pawn; AdminUsers identity refreshed";
                }
                else
                {
                    detail = "reusing synthetic controller and pawn";
                }
                return true;
            }

            auto* controller_class = find_object<UClass>(
                STR("/Game/ConZ_Files/Blueprints/PlayerControllers/BP_ConZPlayerController.BP_ConZPlayerController_C"));
            if (!controller_class)
            {
                detail = "BP_ConZPlayerController class is not loaded";
                return false;
            }

            auto* controller = spawn_actor(current_world, controller_class, detail);
            if (!controller)
            {
                return false;
            }
            disable_actor_tick(controller);

            auto* profile_class = find_object<UClass>(STR("/Script/SCUM.UserProfile"));
            if (profile_class)
            {
                RC::Unreal::FStaticConstructObjectParameters parameters{profile_class, controller};
                auto* profile = RC::Unreal::UObjectGlobals::StaticConstructObject(parameters);
                if (profile)
                {
                    profile->SetFlags(k_mark_as_root_set);
                    if (auto* property = find_property(controller->GetClassPrivate(), STR("_userProfile")))
                    {
                        auto* value = property->ContainerPtrToValuePtr<UObject*>(controller);
                        if (value)
                        {
                            *value = profile;
                        }
                    }
                    best_effort_bind_admin_identity(profile, configured_admin_steam_id);
                    state.user_profile = profile;
                }
            }
            best_effort_bind_admin_identity(controller, configured_admin_steam_id);

            auto* character_class = find_object<UClass>(STR("/Script/SCUM.ConZCharacter"));
            if (!character_class)
            {
                character_class = find_object<UClass>(STR("/Script/ConZ.ConZCharacter"));
            }
            if (!character_class)
            {
                detail = "ConZCharacter class is not loaded";
                return false;
            }

            auto* pawn = spawn_actor(current_world, character_class, detail);
            if (!pawn)
            {
                return false;
            }
            disable_actor_tick(pawn);

            auto* possess = find_object<UFunction>(STR("/Script/Engine.Controller:Possess"));
            const int32_t pawn_offset = property_offset(possess, STR("InPawn"));
            if (!possess || pawn_offset < 0)
            {
                detail = "Controller:Possess or its InPawn parameter is not available";
                return false;
            }
            std::array<uint8_t, k_function_parameter_bytes> parameters{};
            if (!write_parameter(parameters, pawn_offset, pawn) || !call_function(controller, possess, parameters.data()))
            {
                detail = "could not possess the synthetic ConZCharacter";
                return false;
            }

            state.controller = controller;
            state.pawn = pawn;
            state.bound_admin_steam_id = std::string{configured_admin_steam_id};
            state.last_context_detail = "synthetic BP_ConZPlayerController + ConZCharacter ready";
            detail = state.last_context_detail;
            return true;
        }

        auto build_verb_map(OfflineAdminExecutor::RuntimeState& state, std::string& detail) -> bool
        {
            auto* base = find_object<UClass>(STR("/Script/SCUM.AdminCommand"));
            if (!base)
            {
                state.verbs.clear();
                detail = "SCUM.AdminCommand base class is not loaded yet";
                return false;
            }
            if (state.admin_command_base == base && !state.verbs.empty())
            {
                return true;
            }

            std::unordered_map<std::string, UClass*> next;
            RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (!looks_like_UClass(object))
                {
                    return RC::LoopAction::Continue;
                }
                auto* candidate = static_cast<UClass*>(object);
                if (!class_is_child_of(candidate, base))
                {
                    return RC::LoopAction::Continue;
                }
                const auto verb = class_verb(candidate);
                if (!verb.empty())
                {
                    next.try_emplace(verb, candidate);
                }
                return RC::LoopAction::Continue;
            });

            state.admin_command_base = base;
            state.verbs = std::move(next);
            if (state.verbs.empty())
            {
                detail = "AdminCommand subclasses were found but no command verbs could be extracted";
                return false;
            }
            detail = "discovered " + std::to_string(state.verbs.size()) + " AdminCommand verbs";
            return true;
        }

        auto resolve_native_executor(OfflineAdminExecutor::RuntimeState& state, std::string& detail) -> void*
        {
            if (state.native_executor)
            {
                detail = state.native_executor_detail;
                return state.native_executor;
            }
            state.native_executor = scan_main_module_for_executor(state.native_executor_detail);
            detail = state.native_executor_detail;
            return state.native_executor;
        }
    }

    OfflineAdminExecutor::Result OfflineAdminExecutor::execute(std::string_view command,
                                                                 std::string_view configured_admin_steam_id)
    {
        if (!m_enabled)
        {
            return {false, false, "offline AdminCommand dispatcher is disabled"};
        }
        if (!m_unreal_ready || !m_state)
        {
            return {true, false, "offline dispatcher is not ready yet"};
        }

        const auto tokens = tokenize(command);
        if (tokens.empty())
        {
            return {false, false, "empty command"};
        }

        std::string verb_detail;
        if (!build_verb_map(*m_state, verb_detail))
        {
            return {true, false, "offline AdminCommand dispatcher unavailable: " + verb_detail};
        }

        const std::string verb = lowercase(tokens.front());
        const auto command_class_it = m_state->verbs.find(verb);
        if (command_class_it == m_state->verbs.end())
        {
            // The caller can still try a standard UE console command through
            // ScumBridge's server-console fallback.
            return {false, false, "no AdminCommand class for '" + tokens.front() + "'"};
        }

        std::string caller_detail;
        if (!ensure_synthetic_caller(*m_state, configured_admin_steam_id, caller_detail))
        {
            return {true, false, "offline synthetic caller unavailable: " + caller_detail};
        }

        if (!m_native_executor_enabled)
        {
            return {true, false, "offline native executor is disabled by config"};
        }

        std::string executor_detail;
        auto* native_executor = resolve_native_executor(*m_state, executor_detail);
        if (!native_executor)
        {
            return {true, false, "offline native executor unavailable: " + executor_detail};
        }

        RC::Unreal::FStaticConstructObjectParameters construction{command_class_it->second, m_state->controller};
        auto* command_instance = RC::Unreal::UObjectGlobals::StaticConstructObject(construction);
        if (!command_instance)
        {
            return {true, false, "could not construct AdminCommand for '" + tokens.front() + "'"};
        }

        RC::Unreal::TArray<FString> arguments{};
        for (size_t index = 1; index < tokens.size(); ++index)
        {
            const auto argument_wide = widen_utf8(tokens[index]);
            arguments.Add(FString{argument_wide.c_str()});
        }

        bool faulted = false;
        const int64_t native_result = call_native_admin_executor(native_executor, command_instance, &arguments, faulted);
        if (faulted)
        {
            return {true, false, "AdminCommand '" + tokens.front() + "' faulted inside the SCUM native executor"};
        }
        if (native_result == 0)
        {
            return {true, false, "AdminCommand '" + tokens.front() + "' ran but the SCUM executor returned 0"};
        }

        return {true, true, "ok: offline AdminCommand '" + tokens.front() + "' executed via synthetic server caller"};
    }

    OfflineAdminExecutor::Result OfflineAdminExecutor::probe(std::string_view configured_admin_steam_id)
    {
        if (!m_enabled)
        {
            return {true, false, "offline AdminCommand dispatcher is disabled"};
        }
        if (!m_unreal_ready || !m_state)
        {
            return {true, false, "offline dispatcher is not ready yet"};
        }

        std::string verb_detail;
        if (!build_verb_map(*m_state, verb_detail))
        {
            return {true, false, "offline AdminCommand dispatcher unavailable: " + verb_detail};
        }

        std::string caller_detail;
        if (!ensure_synthetic_caller(*m_state, configured_admin_steam_id, caller_detail))
        {
            return {true, false, "offline synthetic caller unavailable: " + caller_detail};
        }

        if (!m_native_executor_enabled)
        {
            return {true, false, "offline native executor is disabled by config"};
        }

        std::string executor_detail;
        if (!resolve_native_executor(*m_state, executor_detail))
        {
            return {true, false, "offline native executor unavailable: " + executor_detail};
        }

        return {true, true, "ok: offline dispatcher ready; " + status()};
    }

    std::string OfflineAdminExecutor::status() const
    {
        std::ostringstream output;
        output << "offline_dispatch=" << (m_enabled ? "enabled" : "disabled")
               << "; native_executor=" << (m_native_executor_enabled ? "enabled" : "disabled");
        if (!m_state)
        {
            return output.str();
        }
        output << "; verbs=" << m_state->verbs.size()
               << "; synthetic=" << (m_state->controller && m_state->pawn ? "ready" : "not-ready")
               << "; context=" << m_state->last_context_detail
               << "; executor=" << m_state->native_executor_detail;
        return output.str();
    }
}
