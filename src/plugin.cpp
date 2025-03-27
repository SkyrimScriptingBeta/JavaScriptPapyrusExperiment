#include <SkyrimScripting/Plugin.h>

void ShowMessageBox(RE::StaticFunctionTag*, std::string_view text) {
    RE::DebugMessageBox(text.data());
}

bool BindFunctions(RE::BSScript::IVirtualMachine* vm) {
    vm->RegisterFunction("ShowMessageBox", "OurScriptName", ShowMessageBox);
    return true;
}

SKSEPlugin_OnDataLoaded { ConsoleLog("Hello from example plugin!"); }

SKSEPlugin_Entrypoint {
    Log("Hello from example plugin!");
    SKSE::GetPapyrusInterface()->Register(BindFunctions);
}