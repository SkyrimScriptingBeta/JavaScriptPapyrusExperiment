#include <SkyrimScripting/Console.h>
#include <SkyrimScripting/Plugin.h>

SkyrimScripting::Console::IConsoleManagerService* consoleManagerService = nullptr;

auto onJavaScriptCommand = function_pointer([](const char* command, const char* commandText,
                                               RE::TESObjectREFR* reference) {
    ConsoleLog("Hello from JS command!!");
    return true;
});

SKSEPlugin_Entrypoint { SkyrimScripting::Console::Initialize(); }

SKSEPlugin_OnPostPostLoad {
    if (consoleManagerService = GetConsoleManager(); consoleManagerService)
        consoleManagerService->add_command_handler("js", &onJavaScriptCommand);
}
