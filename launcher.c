#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_PATH_LEN 32768
#define MAX_CMD_LEN 32768

// Hide console window as early as possible to prevent flash in GUI mode
// This runs before main() via compiler-specific mechanisms
#ifdef _MSC_VER
    // MSVC: Use #pragma to run at startup
    #pragma section(".CRT$XCU", read)
    static void hideConsoleEarly(void) {
        HWND consoleWnd = GetConsoleWindow();
        if (consoleWnd) {
            ShowWindow(consoleWnd, SW_HIDE);
        }
    }
    __declspec(allocate(".CRT$XCU")) void (*hideConsoleEarlyPtr)(void) = hideConsoleEarly;
#else
    // GCC/MinGW: Use constructor attribute
    __attribute__((constructor)) void hideConsoleEarly() {
        HWND consoleWnd = GetConsoleWindow();
        if (consoleWnd) {
            ShowWindow(consoleWnd, SW_HIDE);
        }
    }
#endif

/**
 * Custom Java/JavaW Launcher
 *
 * This launcher intelligently chooses between java.exe and javaw.exe based on execution context:
 * - If run from console/terminal -> uses java.exe (shows console output)
 * - If double-clicked from Explorer -> uses javaw.exe (no console window)
 *
 * Features:
 * - Auto-detects Java in PATH
 * - Supports --java-home override
 * - Forwards all arguments and exit codes
 * - Proper error reporting
 */

// Function to find java executable in PATH
int findJavaInPath(const char* exeName, char* outPath, size_t outPathSize) {
    char* pathEnv = getenv("PATH");
    if (!pathEnv) {
        return 0;
    }

    // Make a copy since strtok modifies the string
    char* pathCopy = _strdup(pathEnv);
    if (!pathCopy) {
        return 0;
    }

    char* token = strtok(pathCopy, ";");
    while (token) {
        char testPath[MAX_PATH];
        snprintf(testPath, sizeof(testPath), "%s\\%s", token, exeName);

        // Check if file exists
        DWORD attrib = GetFileAttributesA(testPath);
        if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            strncpy(outPath, testPath, outPathSize - 1);
            outPath[outPathSize - 1] = '\0';
            free(pathCopy);
            return 1;
        }

        token = strtok(NULL, ";");
    }

    free(pathCopy);
    return 0;
}

// Function to detect if we're in GUI mode (double-clicked from Explorer)
// Returns: TRUE if GUI mode (should use javaw.exe), FALSE if console mode
BOOL isGuiMode() {
    // With CONSOLE subsystem, Windows already created a console for us
    // We need to detect if we were launched from a terminal (console mode)
    // or double-clicked from Explorer (GUI mode)

    // First, hide the console window to prevent flashing in GUI mode
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, SW_HIDE);
    }

    // Free our current console so we can try to attach to parent
    FreeConsole();

    // Now try to attach to parent console
    BOOL couldAttach = AttachConsole(ATTACH_PARENT_PROCESS);

    if (couldAttach) {
        // We could attach to parent console - we're in console mode!
        // Keep attached so Java can inherit it
        // Make console visible again (it's the parent's console now)
        consoleWnd = GetConsoleWindow();
        if (consoleWnd) {
            ShowWindow(consoleWnd, SW_SHOW);
        }
        return FALSE;  // Console mode
    }

    // Couldn't attach to parent console - we were double-clicked from Explorer
    // Console stays hidden (already hidden above)
    return TRUE;  // GUI mode
}

// Function to parse --java-home argument
char* extractJavaHome(const char* cmdLine) {
    const char* javaHomeArg = strstr(cmdLine, "--java-home=");
    if (!javaHomeArg) {
        javaHomeArg = strstr(cmdLine, "--java-home ");
        if (javaHomeArg) {
            javaHomeArg += 12; // Skip "--java-home "
            // Find the end (next space or quote)
            const char* end = strchr(javaHomeArg, ' ');
            if (!end) end = javaHomeArg + strlen(javaHomeArg);

            size_t len = end - javaHomeArg;
            char* result = (char*)malloc(len + 1);
            if (result) {
                strncpy(result, javaHomeArg, len);
                result[len] = '\0';
            }
            return result;
        }
        return NULL;
    }

    javaHomeArg += 12; // Skip "--java-home="

    // Handle quoted paths
    if (*javaHomeArg == '"') {
        javaHomeArg++;
        const char* endQuote = strchr(javaHomeArg, '"');
        if (!endQuote) return NULL;

        size_t len = endQuote - javaHomeArg;
        char* result = (char*)malloc(len + 1);
        if (result) {
            strncpy(result, javaHomeArg, len);
            result[len] = '\0';
        }
        return result;
    }

    // Unquoted path - find next space
    const char* end = strchr(javaHomeArg, ' ');
    if (!end) end = javaHomeArg + strlen(javaHomeArg);

    size_t len = end - javaHomeArg;
    char* result = (char*)malloc(len + 1);
    if (result) {
        strncpy(result, javaHomeArg, len);
        result[len] = '\0';
    }
    return result;
}

// Function to remove --java-home from command line
void removeJavaHomeArg(char* cmdLine) {
    char* javaHomeArg = strstr(cmdLine, "--java-home");
    if (!javaHomeArg) return;

    // Find the end of the argument
    char* argEnd = javaHomeArg;
    if (strncmp(javaHomeArg, "--java-home=", 12) == 0) {
        argEnd += 12;
        // Skip quoted or unquoted path
        if (*argEnd == '"') {
            argEnd = strchr(argEnd + 1, '"');
            if (argEnd) argEnd++;
        } else {
            while (*argEnd && *argEnd != ' ') argEnd++;
        }
    } else {
        argEnd += 11; // Skip "--java-home"
        while (*argEnd == ' ') argEnd++; // Skip spaces
        // Skip the path value
        if (*argEnd == '"') {
            argEnd = strchr(argEnd + 1, '"');
            if (argEnd) argEnd++;
        } else {
            while (*argEnd && *argEnd != ' ') argEnd++;
        }
    }

    // Skip trailing space
    if (*argEnd == ' ') argEnd++;

    // Remove by shifting the rest of the string
    memmove(javaHomeArg, argEnd, strlen(argEnd) + 1);
}

// Function to show message appropriately (console or GUI)
void showMessage(BOOL hasConsole, const char* title, const char* message, UINT type) {
    if (hasConsole) {
        // Console mode - use printf
        if (type == MB_ICONERROR) {
            printf("\n[ERROR] %s\n", title);
        } else if (type == MB_ICONINFORMATION) {
            printf("\n[INFO] %s\n", title);
        } else {
            printf("\n%s\n", title);
        }
        printf("%s\n\n", message);
    } else {
        // GUI mode - use MessageBox
        MessageBoxA(NULL, message, title, type);
    }
}

int main(int argc, char** argv) {
    char javaPath[MAX_PATH] = {0};
    char cmdLine[MAX_CMD_LEN];

    // Detect if we're in GUI mode (double-clicked) or console mode (terminal)
    BOOL guiMode = isGuiMode();
    BOOL hasConsole = !guiMode;
    const char* javaExeName = hasConsole ? "java.exe" : "javaw.exe";

    // Get full command line
    LPSTR fullCmdLine = GetCommandLineA();
    strncpy(cmdLine, fullCmdLine, sizeof(cmdLine) - 1);
    cmdLine[sizeof(cmdLine) - 1] = '\0';

    // Check for --java-home override
    char* javaHome = extractJavaHome(cmdLine);

    if (javaHome) {
        // Use the specified Java home
        snprintf(javaPath, sizeof(javaPath), "%s\\bin\\%s", javaHome, javaExeName);

        // Verify the path exists
        DWORD attrib = GetFileAttributesA(javaPath);
        if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            char error[1024];
            snprintf(error, sizeof(error),
                     "Java not found at specified location:\n%s\n\nPlease check your --java-home path.",
                     javaPath);
            showMessage(hasConsole, "Java Not Found", error, MB_ICONERROR);
            free(javaHome);
            return 1;
        }

        free(javaHome);
        removeJavaHomeArg(cmdLine);
    } else {
        // Try to find Java in PATH
        if (!findJavaInPath(javaExeName, javaPath, sizeof(javaPath))) {
            char error[1024];
            snprintf(error, sizeof(error),
                     "Java not found in PATH.\n\n"
                     "Please ensure Java is installed and added to PATH,\n"
                     "or use --java-home=C:\\path\\to\\jdk to specify location.\n\n"
                     "Looking for: %s",
                     javaExeName);
            showMessage(hasConsole, "Java Not Found", error, MB_ICONERROR);
            return 1;
        }
    }

    // Skip past the executable name in command line
    char* jarArgs = cmdLine;
    if (*jarArgs == '"') {
        jarArgs = strchr(jarArgs + 1, '"');
        if (jarArgs) jarArgs += 2; // Skip quote and space
    } else {
        jarArgs = strchr(jarArgs, ' ');
        if (jarArgs) jarArgs++; // Skip space
    }

    // Trim leading spaces
    while (jarArgs && *jarArgs == ' ') jarArgs++;

    if (!jarArgs || !*jarArgs) {
        // Show diagnostic information when no JAR is provided
        char info[2048];
        snprintf(info, sizeof(info),
                 "Execution Context: %s\n"
                 "Java Executable: %s\n"
                 "Java Location: %s\n"
                 "Status: Java detected successfully\n\n"
                 "Usage: jarrunner.exe [--java-home=PATH] <jar-file> [args...]\n\n"
                 "Examples:\n"
                 "  jarrunner.exe myapp.jar\n"
                 "  jarrunner.exe --java-home=C:\\Java\\jdk21 myapp.jar --arg1 value1",
                 hasConsole ? "Console (terminal/cmd)" : "GUI (double-clicked)",
                 javaExeName,
                 javaPath);
        showMessage(hasConsole, "Java Runner - Diagnostic Info", info, MB_ICONINFORMATION);
        return 1;
    }

    // Build final command line: "full\path\to\java.exe" -jar <remaining args>
    char finalCmdLine[MAX_CMD_LEN];
    snprintf(finalCmdLine, sizeof(finalCmdLine), "\"%s\" -jar %s", javaPath, jarArgs);

    // Setup startup info
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    if (hasConsole) {
        // In console mode, explicitly pass the console handles
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    // Inherit handles so console I/O works
    if (CreateProcessA(NULL, finalCmdLine, NULL, NULL, TRUE,
                      0, NULL, NULL, &si, &pi)) {

        if (hasConsole) {
            // Console mode: Wait for Java process to complete
            // User expects synchronous behavior and proper exit codes
            WaitForSingleObject(pi.hProcess, INFINITE);

            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            return exitCode;
        } else {
            // GUI mode: Launch and exit immediately
            // Let javaw.exe run independently without keeping launcher alive
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            return 0;
        }
    }

    // If we get here, CreateProcess failed
    char error[2048];
    DWORD lastError = GetLastError();
    snprintf(error, sizeof(error),
             "Failed to launch Java process.\n\n"
             "Java: %s\n"
             "Command: %s\n"
             "Error code: %lu\n\n"
             "Make sure Java is properly installed.",
             javaPath, finalCmdLine, lastError);
    showMessage(hasConsole, "Launch Error", error, MB_ICONERROR);

    // Don't call FreeConsole() - causes prompt to appear prematurely
    return 1;
}
