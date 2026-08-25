#include "SimpleRconMod.hpp"

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new simple_rcon::SimpleRconMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
