#include <SkyrimScripting/Console.h>
#include <SkyrimScripting/Plugin.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>

#include "quickjs.h"

using namespace std;

// Store dynamically created globals
unordered_map<string, JSValue> global_vars;

// Flag to check if CTRL+C was pressed
volatile sig_atomic_t ctrl_c_pressed = 0;

// Signal handler for CTRL+C
void handle_signal(int signal) {
    if (signal == SIGINT) {
        ctrl_c_pressed = 1;
    }
}

// C++ function exposed to JS
JSValue js_lookup_global(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Log("C++ function called from JS");

    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_UNDEFINED;
    }

    // Get the global name
    size_t      len;
    const char* prop_name = JS_ToCStringLen(ctx, &len, argv[0]);

    if (!prop_name) {
        return JS_UNDEFINED;
    } else {
        Log("Looking up global: {}", prop_name);
    }

    // Check if we already created this global
    auto it = global_vars.find(prop_name);
    if (it != global_vars.end()) {
        JS_FreeCString(ctx, prop_name);
        return JS_DupValue(ctx, it->second);
    }

    // If the prop name is "MyString" then lazily define a global string with the value "I am a
    // string!"
    if (strcmp(prop_name, "MyString") == 0) {
        JSValue new_global     = JS_NewString(ctx, "I am a string!");
        global_vars[prop_name] = JS_DupValue(ctx, new_global);

        // Define on globalThis so it persists
        JSValue global_obj = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global_obj, prop_name, JS_DupValue(ctx, new_global));
        JS_FreeValue(ctx, global_obj);

        JS_FreeCString(ctx, prop_name);
        return new_global;
    }

    // Lazy define it (for example, defaulting to an empty object)
    JSValue new_global     = JS_UNDEFINED;
    global_vars[prop_name] = JS_DupValue(ctx, new_global);

    // Define on globalThis so it persists
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global_obj, prop_name, JS_DupValue(ctx, new_global));
    JS_FreeValue(ctx, global_obj);

    Log("Lazy defined global: {}", prop_name);

    JS_FreeCString(ctx, prop_name);
    return new_global;
}

// Error handling helper function
static void js_dump_error(JSContext* ctx) {
    JSValue     exception = JS_GetException(ctx);
    const char* error_msg = JS_ToCString(ctx, exception);
    Log("Error: {}", error_msg ? error_msg : "unknown");
    if (error_msg) JS_FreeCString(ctx, error_msg);
    JS_FreeValue(ctx, exception);
}

// Custom console.log implementation
static JSValue js_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    string output;
    for (int i = 0; i < argc; i++) {
        const char* str = JS_ToCString(ctx, argv[i]);
        if (str) {
            output += str;
            JS_FreeCString(ctx, str);
        }
        if (i < argc - 1) output += " ";
    }
    Log("{}", output);
    ConsoleLog(output.c_str());
    return JS_UNDEFINED;
}

void setup_js_env(JSContext* ctx) {
    // Get global object
    JSValue global_obj = JS_GetGlobalObject(ctx);

    // Create a QuickJS function that JS can call
    JS_SetPropertyStr(
        ctx, global_obj, "__lookup_global_from_cpp",
        JS_NewCFunction(ctx, js_lookup_global, "__lookup_global_from_cpp", 1)
    );

    // Inject JavaScript code to override globalThis with a Proxy
    const char* proxy_setup_code = R"(
        (function() {
            const nativeGlobalLookup = (name) => __lookup_global_from_cpp(name);

            globalThis = new Proxy(globalThis, {
                get(target, prop, receiver) {
                    if (!(prop in target)) {
                        return nativeGlobalLookup(prop);
                    }
                    return Reflect.get(target, prop, receiver);
                }
            });
        })();
    )";

    JSValue eval_result = JS_Eval(
        ctx, proxy_setup_code, strlen(proxy_setup_code), "<proxy-setup>", JS_EVAL_TYPE_GLOBAL
    );

    if (JS_IsException(eval_result)) {
        Log("Failed to setup Proxy for globalThis");
        js_dump_error(ctx);
    }

    JS_FreeValue(ctx, eval_result);
    JS_FreeValue(ctx, global_obj);
}

// Global variables for QuickJS environment
JSRuntime*  runtime = nullptr;
JSContext*  context = nullptr;
std::string input_buffer;
bool        empty_line_detected = false;

// Initialize JS environment
void initialize_js_environment() {
    // Initialize QuickJS runtime with proper memory limits
    runtime = JS_NewRuntime();
    if (!runtime) {
        ConsoleLog("Failed to create JS runtime");
        return;
    }

    // Set memory limit (in bytes) to avoid overflow issues
    JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);  // 64 MB

    // Set maximum stack size
    JS_SetMaxStackSize(runtime, 1024 * 1024);  // 1 MB

    // Create a JavaScript context
    context = JS_NewContext(runtime);
    if (!context) {
        ConsoleLog("Failed to create JS context");
        JS_FreeRuntime(runtime);
        runtime = nullptr;
        return;
    }

    // Create global object manually instead of using JS_AddIntrinsicBaseObjects
    JSValue global = JS_GetGlobalObject(context);
    if (JS_IsException(global)) {
        ConsoleLog("Failed to get global object");
        js_dump_error(context);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        context = nullptr;
        runtime = nullptr;
        return;
    }

    // Setup custom environment
    setup_js_env(context);

    // Add a console object with log method
    JSValue console = JS_NewObject(context);
    JS_SetPropertyStr(context, console, "log", JS_NewCFunction(context, js_console_log, "log", 1));
    JS_SetPropertyStr(context, global, "console", console);

    // Free the global object reference
    JS_FreeValue(context, global);

    ConsoleLog("JavaScript environment initialized");
}

// Cleanup JS environment
void cleanup_js_environment() {
    if (context) {
        JS_FreeContext(context);
        context = nullptr;
    }

    if (runtime) {
        JS_FreeRuntime(runtime);
        runtime = nullptr;
    }

    // Clear the input buffer
    input_buffer.clear();
    empty_line_detected = false;
}

// Execute the JavaScript code in the input buffer
void execute_js_code() {
    if (!context || input_buffer.empty()) return;

    PrintToConsole("Executing JavaScript code:");

    std::string wrapped_code =
        "(function() {\n"
        "  try {\n"
        "    return eval(`" +
        input_buffer +
        "`);\n"
        "  } catch (e) {\n"
        "    if (e instanceof ReferenceError && e.message.includes('is not defined')) {\n"
        "      const varName = e.message.split(' ')[0];\n"
        "      globalThis[varName] = __lookup_global_from_cpp(varName);\n"
        "      // Try again with the defined variable\n"
        "      return eval(`" +
        input_buffer +
        "`);\n"
        "    }\n"
        "    throw e;\n"
        "  }\n"
        "})()";

    JSValue result = JS_Eval(
        context, wrapped_code.c_str(), wrapped_code.length(), "<skyrim-console>",
        JS_EVAL_TYPE_GLOBAL
    );

    // Check for errors
    if (JS_IsException(result)) {
        js_dump_error(context);
    } else if (!JS_IsUndefined(result)) {
        // Print the result if it's not undefined
        const char* result_str = JS_ToCString(context, result);
        if (result_str) {
            PrintToConsole("=> {}", result_str);
            JS_FreeCString(context, result_str);
        }
    }

    // Free the result value
    JS_FreeValue(context, result);

    // Reset input buffer after execution
    input_buffer.clear();
}

std::atomic<bool> _isJavaScriptREPLRunning = false;

SkyrimScripting::Console::IConsoleManagerService* consoleManagerService = nullptr;

constexpr auto START_REPL_COMMAND = "js";
constexpr auto END_REPL_COMMAND   = "end";
constexpr auto QUIT_GAME_COMMAND  = "qqq";

auto onJavaScriptREPLText =
    function_pointer([](const char* commandText, RE::TESObjectREFR* reference) {
        Log("Received command: {}", commandText);
        if (_isJavaScriptREPLRunning) {
            std::string current_line(commandText);

            if (current_line == QUIT_GAME_COMMAND) return false;

            if (current_line == END_REPL_COMMAND) {
                Log("Ending JavaScript REPL...");
                ConsoleLog("Ending JavaScript REPL...");

                // Clean up the JavaScript environment
                cleanup_js_environment();

                _isJavaScriptREPLRunning = false;
                consoleManagerService->release_ownership();
                return true;
            }

            // Check if the line is empty or all whitespace
            if (current_line.empty() || current_line == "\r" ||
                all_of(current_line.begin(), current_line.end(), [](unsigned char c) {
                    return isspace(c);
                })) {
                if (empty_line_detected) {
                    // Double newline detected, evaluate the code
                    Log("Executing JavaScript code: {}", input_buffer);
                    execute_js_code();
                    empty_line_detected = false;
                } else {
                    empty_line_detected = true;
                }
            } else {
                if (!input_buffer.empty()) input_buffer += "\n";
                input_buffer += current_line;
                empty_line_detected = false;
                Log("{}", current_line);
            }
            return true;
        }
        Log("JavaScript REPL is not running, ignoring command.");
        return false;
    });

auto onStartJavaScriptREPL = function_pointer([](const char* command, const char* commandText,
                                                 RE::TESObjectREFR* reference) {
    if (!_isJavaScriptREPLRunning) {
        Log("Starting JavaScript REPL...");
        ConsoleLog("Starting JavaScript REPL...");

        // Initialize the JavaScript environment
        initialize_js_environment();

        // Clear input state
        input_buffer.clear();
        empty_line_detected = false;

        consoleManagerService->claim_ownership(&onJavaScriptREPLText);
        _isJavaScriptREPLRunning = true;

        PrintToConsole(
            "QuickJS REPL - Enter JavaScript code (double newline to execute, use 'end' command to "
            "exit)"
        );
        PrintToConsole("> ");

        return true;
    }
    return false;
});

// auto onEndJavaScriptREPL = function_pointer([](const char* command, const char* commandText,
//                                                RE::TESObjectREFR* reference) {
//     if (_isJavaScriptREPLRunning) {
//         Log("Ending JavaScript REPL...");
//         ConsoleLog("Ending JavaScript REPL...");

//         // Clean up the JavaScript environment
//         cleanup_js_environment();

//         _isJavaScriptREPLRunning = false;
//         consoleManagerService->release_ownership(&onJavaScriptREPLText);
//         return true;
//     }
//     return false;
// });

SKSEPlugin_Entrypoint {
    Log("Plugin loaded successfully!");
    SkyrimScripting::Console::Initialize();
}

SKSEPlugin_OnPostPostLoad {
    if (consoleManagerService = GetConsoleManager(); consoleManagerService) {
        consoleManagerService->add_command_handler(START_REPL_COMMAND, &onStartJavaScriptREPL);
    }
}
