#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_PATH_LEN 32768
#define MAX_CMD_LEN 32768
#define MAX_CONFIG_LINE 4096

// Configuration structure
typedef struct {
    char vmArgs[MAX_CMD_LEN];      // VM arguments (before -jar)
    char javaArgs[MAX_CMD_LEN];    // Java arguments (-jar, -cp, main class, etc.)
    char appArgs[MAX_CMD_LEN];     // Application arguments (after jar/class)
    char logFile[MAX_PATH];        // Log file path
    char logLevel[32];             // Log level: info, warning, error, none
    int logOverwrite;              // Overwrite log file (1) or append (0)
    int enableAOT;                 // Enable AOT cache (1=yes, 0=no, -1=not specified)
} LauncherConfig;

// Global log file handle
static FILE* g_logFile = NULL;
static int g_logEnabled = 0;

// Global timing variables
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_startTime;

// Base52 encoding (alphanumeric, case-sensitive without confusing chars)
// Using: 0-9, A-Z (except I, O), a-z (except l, o)
static const char BASE52_CHARS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";

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
 * Java Runner (jr) - Smart Java/JavaW Launcher
 *
 * Features:
 * - Auto-detects console vs GUI mode (java.exe vs javaw.exe)
 * - Config file support (.jrc) for renamed executables
 * - AOT cache support (JDK 25+) with auto-management
 * - Flexible configuration (VM args, Java args, App args)
 * - Optional debug logging
 * - Performance timing measurements
 */

// Initialize high-resolution timer
void initTimer() {
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_startTime);
}

// Get elapsed microseconds since start
long long getElapsedMicros() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return ((now.QuadPart - g_startTime.QuadPart) * 1000000LL) / g_perfFreq.QuadPart;
}

// Logging functions
void initLog(const char* logPath, int overwrite) {
    if (!logPath || !*logPath) {
        g_logEnabled = 0;
        return;
    }

    const char* mode = overwrite ? "w" : "a";
    g_logFile = fopen(logPath, mode);
    if (g_logFile) {
        g_logEnabled = 1;

        // Write header
        time_t now = time(NULL);
        char timebuf[64];
        struct tm* tm_info = localtime(&now);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(g_logFile, "\n========================================\n");
        fprintf(g_logFile, "Java Runner Log - %s\n", timebuf);
        fprintf(g_logFile, "========================================\n");
        fflush(g_logFile);
    }
}

void writeLog(const char* level, const char* format, ...) {
    if (!g_logEnabled || !g_logFile) return;

    va_list args;
    va_start(args, format);

    fprintf(g_logFile, "[%s] ", level);
    vfprintf(g_logFile, format, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);

    va_end(args);
}

void closeLog() {
    if (g_logFile) {
        fprintf(g_logFile, "========================================\n\n");
        fclose(g_logFile);
        g_logFile = NULL;
        g_logEnabled = 0;
    }
}

// Trim whitespace from string (in-place)
void trim(char* str) {
    if (!str || !*str) return;

    // Trim leading spaces
    char* start = str;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
        start++;
    }

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }

    // Trim trailing spaces
    char* end = str + strlen(str) - 1;
    while (end >= str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
}

// Encode 64-bit number to base52 string
void encodeBase52(unsigned long long value, char* output, size_t maxLen) {
    if (maxLen < 2) return;

    if (value == 0) {
        output[0] = BASE52_CHARS[0];
        output[1] = '\0';
        return;
    }

    char temp[32];
    int pos = 0;

    while (value > 0 && pos < 31) {
        temp[pos++] = BASE52_CHARS[value % 52];
        value /= 52;
    }

    // Reverse the string
    int i;
    for (i = 0; i < pos && i < (int)maxLen - 1; i++) {
        output[i] = temp[pos - 1 - i];
    }
    output[i] = '\0';
}

// Get file size and last modified time
int getFileInfo(const char* path, unsigned long long* size, unsigned long long* modTime) {
    struct _stat64 st;
    if (_stat64(path, &st) != 0) {
        return 0;
    }
    *size = (unsigned long long)st.st_size;
    *modTime = (unsigned long long)st.st_mtime;
    return 1;
}

// Build AOT cache filename: <jarname>.<size_base52>.<modtime_base52>.aot
void buildAOTCacheName(const char* jarPath, char* aotPath, size_t aotPathSize) {
    unsigned long long size, modTime;
    if (!getFileInfo(jarPath, &size, &modTime)) {
        aotPath[0] = '\0';
        return;
    }

    // Extract directory and filename without extension
    char dirPath[MAX_PATH];
    char baseName[MAX_PATH];

    const char* lastSlash = strrchr(jarPath, '\\');
    if (!lastSlash) lastSlash = strrchr(jarPath, '/');

    if (lastSlash) {
        size_t dirLen = lastSlash - jarPath;
        strncpy(dirPath, jarPath, dirLen);
        dirPath[dirLen] = '\0';
        strcpy(baseName, lastSlash + 1);
    } else {
        dirPath[0] = '\0';
        strcpy(baseName, jarPath);
    }

    // Remove .jar extension
    char* dotPos = strrchr(baseName, '.');
    if (dotPos) *dotPos = '\0';

    // Encode size and modTime to base52
    char sizeStr[32], modTimeStr[32];
    encodeBase52(size, sizeStr, sizeof(sizeStr));
    encodeBase52(modTime, modTimeStr, sizeof(modTimeStr));

    // Build final path
    if (dirPath[0]) {
        snprintf(aotPath, aotPathSize, "%s\\%s.%s.%s.aot",
                 dirPath, baseName, sizeStr, modTimeStr);
    } else {
        snprintf(aotPath, aotPathSize, "%s.%s.%s.aot",
                 baseName, sizeStr, modTimeStr);
    }
}

// Delete outdated AOT cache files for the given JAR
void cleanupOldAOTFiles(const char* jarPath, const char* currentAOTPath) {
    char dirPath[MAX_PATH];
    char baseName[MAX_PATH];
    char pattern[MAX_PATH];

    // Extract directory and base filename
    const char* lastSlash = strrchr(jarPath, '\\');
    if (!lastSlash) lastSlash = strrchr(jarPath, '/');

    if (lastSlash) {
        size_t dirLen = lastSlash - jarPath;
        strncpy(dirPath, jarPath, dirLen);
        dirPath[dirLen] = '\0';
        strcpy(baseName, lastSlash + 1);
    } else {
        GetCurrentDirectoryA(sizeof(dirPath), dirPath);
        strcpy(baseName, jarPath);
    }

    // Remove .jar extension
    char* dotPos = strrchr(baseName, '.');
    if (dotPos) *dotPos = '\0';

    // Build search pattern: <baseName>.*.aot
    snprintf(pattern, sizeof(pattern), "%s\\%s.*.aot", dirPath, baseName);

    // Find all matching AOT files
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern, &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        // Extract filename from currentAOTPath for comparison
        const char* currentFileName = strrchr(currentAOTPath, '\\');
        if (!currentFileName) currentFileName = strrchr(currentAOTPath, '/');
        if (currentFileName) {
            currentFileName++;
        } else {
            currentFileName = currentAOTPath;
        }

        do {
            // Delete if it's not the current AOT file (compare filenames only)
            if (_stricmp(findData.cFileName, currentFileName) != 0) {
                char fullPath[MAX_PATH];
                snprintf(fullPath, sizeof(fullPath), "%s\\%s", dirPath, findData.cFileName);
                DeleteFileA(fullPath);
                writeLog("INFO", "Cleaned up old AOT file: %s", fullPath);
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }
}

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
    writeLog(type == MB_ICONERROR ? "ERROR" : "INFO", "%s: %s", title, message);
}

// Parse config file (.jrc format)
// Returns 1 on success, 0 on failure
int parseConfigFile(const char* configPath, LauncherConfig* config) {
    FILE* f = fopen(configPath, "r");
    if (!f) return 0;

    writeLog("INFO", "Loading config file: %s", configPath);

    // Initialize config with defaults
    memset(config, 0, sizeof(LauncherConfig));
    config->enableAOT = -1;  // Not specified (use default or cmdline)
    config->logOverwrite = 0; // Append by default
    strcpy(config->logLevel, "info");

    char line[MAX_CONFIG_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);

        // Skip empty lines and comments
        if (!*line || *line == '#') continue;

        // Look for key=value
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* key = line;
        char* value = eq + 1;

        trim(key);
        trim(value);

        // Parse known keys (matching WinRun4J/jpackage style)
        if (_stricmp(key, "vm.args") == 0) {
            strncpy(config->vmArgs, value, sizeof(config->vmArgs) - 1);
            writeLog("INFO", "vm.args=%s", value);
        } else if (_stricmp(key, "java.args") == 0) {
            strncpy(config->javaArgs, value, sizeof(config->javaArgs) - 1);
            writeLog("INFO", "java.args=%s", value);
        } else if (_stricmp(key, "app.args") == 0) {
            strncpy(config->appArgs, value, sizeof(config->appArgs) - 1);
            writeLog("INFO", "app.args=%s", value);
        } else if (_stricmp(key, "log.file") == 0) {
            strncpy(config->logFile, value, sizeof(config->logFile) - 1);
        } else if (_stricmp(key, "log.level") == 0) {
            strncpy(config->logLevel, value, sizeof(config->logLevel) - 1);
        } else if (_stricmp(key, "log.overwrite") == 0) {
            config->logOverwrite = (_stricmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (_stricmp(key, "aot") == 0) {
            if (_stricmp(value, "true") == 0 || strcmp(value, "1") == 0) {
                config->enableAOT = 1;
                writeLog("INFO", "aot=true");
            } else if (_stricmp(value, "false") == 0 || strcmp(value, "0") == 0) {
                config->enableAOT = 0;
                writeLog("INFO", "aot=false");
            }
        }
    }

    fclose(f);
    return 1;
}

// Create a sample config file
int createConfigFile(const char* configPath, const char* jarPath) {
    FILE* f = fopen(configPath, "w");
    if (!f) return 0;

    fprintf(f, "# Java Runner Configuration (.jrc format)\n");
    fprintf(f, "# Lines starting with # are comments\n");
    fprintf(f, "# Format follows WinRun4J/jpackage conventions\n\n");

    fprintf(f, "# VM arguments (passed before -jar, launcher auto-injects AOT flags here)\n");
    fprintf(f, "#vm.args=-Xmx512m -Xms128m -Dapp.mode=production\n\n");

    fprintf(f, "# Java arguments (everything after VM args: -jar, -cp, class name, etc.)\n");
    if (jarPath && *jarPath) {
        fprintf(f, "java.args=-jar %s\n\n", jarPath);
    } else {
        fprintf(f, "#java.args=-jar yourapp.jar\n");
        fprintf(f, "# Or for classpath: java.args=-cp lib/*:app.jar com.example.Main\n\n");
    }

    fprintf(f, "# Application arguments (passed to your main method)\n");
    fprintf(f, "#app.args=--config myconfig.xml --verbose\n\n");

    fprintf(f, "# AOT cache control (optional, default: true)\n");
    fprintf(f, "#aot=true\n\n");

    fprintf(f, "# Debug logging (optional, only used when specified)\n");
    fprintf(f, "#log.file=launcher.log\n");
    fprintf(f, "#log.level=info\n");
    fprintf(f, "#log.overwrite=false\n");

    fclose(f);
    return 1;
}

// Get the executable's own filename (without .exe extension)
void getExeBaseName(char* baseName, size_t size) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));

    // Get just the filename
    const char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = strrchr(exePath, '/');
    const char* fileName = lastSlash ? lastSlash + 1 : exePath;

    // Copy and remove .exe extension
    strncpy(baseName, fileName, size - 1);
    baseName[size - 1] = '\0';

    char* dotPos = strrchr(baseName, '.');
    if (dotPos && _stricmp(dotPos, ".exe") == 0) {
        *dotPos = '\0';
    }
}

// Get the executable's full path (without .exe extension)
// This is used for finding the .jrc config file in the same directory as the .exe
void getExeFullPathWithoutExt(char* fullPath, size_t size) {
    GetModuleFileNameA(NULL, fullPath, size);
    fullPath[size - 1] = '\0';

    // Remove .exe extension
    char* dotPos = strrchr(fullPath, '.');
    if (dotPos && _stricmp(dotPos, ".exe") == 0) {
        *dotPos = '\0';
    }
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

// Extract JAR file path from command line arguments
void extractJarPath(const char* args, char* jarPath, size_t jarPathSize) {
    if (!args || !*args) {
        jarPath[0] = '\0';
        return;
    }

    const char* jarStart = strstr(args, "-jar ");
    if (!jarStart) {
        jarPath[0] = '\0';
        return;
    }

    jarStart += 5; // Skip "-jar "
    while (*jarStart == ' ') jarStart++; // Skip spaces

    // Parse JAR file path (handle quoted and unquoted paths)
    if (*jarStart == '"') {
        jarStart++;
        const char* jarEnd = strchr(jarStart, '"');
        if (jarEnd) {
            size_t len = jarEnd - jarStart;
            if (len < jarPathSize) {
                strncpy(jarPath, jarStart, len);
                jarPath[len] = '\0';
                return;
            }
        }
    } else {
        const char* jarEnd = strchr(jarStart, ' ');
        if (!jarEnd) jarEnd = jarStart + strlen(jarStart);
        size_t len = jarEnd - jarStart;
        if (len < jarPathSize) {
            strncpy(jarPath, jarStart, len);
            jarPath[len] = '\0';
            return;
        }
    }

    jarPath[0] = '\0';
}

int main(int argc, char** argv) {
    char javaPath[MAX_PATH] = {0};
    char exeBaseName[MAX_PATH] = {0};
    char configPath[MAX_PATH] = {0};
    LauncherConfig config;
    int useConfig = 0;

    // Initialize high-resolution timer
    initTimer();
    long long startTimeMicros = getElapsedMicros();

    // Get executable base name (without .exe) - for display purposes
    getExeBaseName(exeBaseName, sizeof(exeBaseName));

    // Build config file path - use full path so it works from any directory
    getExeFullPathWithoutExt(configPath, sizeof(configPath));
    strncat(configPath, ".jrc", sizeof(configPath) - strlen(configPath) - 1);

    // Try to load config file
    useConfig = parseConfigFile(configPath, &config);

    // Initialize logging if configured
    if (useConfig && config.logFile[0]) {
        initLog(config.logFile, config.logOverwrite);
        writeLog("INFO", "Launcher started: %s.exe", exeBaseName);
    }

    // Detect if we're in GUI mode (double-clicked) or console mode (terminal)
    BOOL guiMode = isGuiMode();
    BOOL hasConsole = !guiMode;
    const char* javaExeName = hasConsole ? "java.exe" : "javaw.exe";

    writeLog("INFO", "Execution mode: %s", hasConsole ? "Console" : "GUI");
    writeLog("INFO", "Java executable: %s", javaExeName);

    // Get full command line
    LPSTR fullCmdLine = GetCommandLineA();

    // Check for --create-config flag
    if (strstr(fullCmdLine, "--create-config")) {
        // Extract JAR path if provided
        char jarPath[MAX_PATH] = {0};
        char* jarArg = strstr(fullCmdLine, "--create-config");
        jarArg += 15; // Skip "--create-config"
        while (*jarArg == ' ') jarArg++;

        if (*jarArg && *jarArg != '-') {
            // JAR path provided
            if (*jarArg == '"') {
                jarArg++;
                char* endQuote = strchr(jarArg, '"');
                if (endQuote) {
                    size_t len = endQuote - jarArg;
                    if (len < sizeof(jarPath)) {
                        strncpy(jarPath, jarArg, len);
                        jarPath[len] = '\0';
                    }
                }
            } else {
                char* end = strchr(jarArg, ' ');
                if (!end) end = jarArg + strlen(jarArg);
                size_t len = end - jarArg;
                if (len < sizeof(jarPath)) {
                    strncpy(jarPath, jarArg, len);
                    jarPath[len] = '\0';
                }
            }
        }

        // Create config file
        if (createConfigFile(configPath, jarPath[0] ? jarPath : NULL)) {
            char msg[1024];
            snprintf(msg, sizeof(msg), "Created config file: %s\n\nEdit this file to customize launcher behavior.", configPath);
            showMessage(hasConsole, "Config Created", msg, MB_ICONINFORMATION);
            closeLog();
            return 0;
        } else {
            char msg[1024];
            snprintf(msg, sizeof(msg), "Failed to create config file: %s", configPath);
            showMessage(hasConsole, "Error", msg, MB_ICONERROR);
            closeLog();
            return 1;
        }
    }

    // Determine AOT setting (priority: cmdline > config > default)
    int enableAOT = 1; // Default: enabled
    if (strstr(fullCmdLine, "--disable-aot")) {
        enableAOT = 0;
    } else if (strstr(fullCmdLine, "--enable-aot")) {
        enableAOT = 1;
    } else if (useConfig && config.enableAOT != -1) {
        enableAOT = config.enableAOT;
    }

    writeLog("INFO", "AOT enabled: %s", enableAOT ? "true" : "false");

    // Check for --java-home override
    char* javaHome = extractJavaHome(fullCmdLine);

    if (javaHome) {
        // Use the specified Java home
        snprintf(javaPath, sizeof(javaPath), "%s\\bin\\%s", javaHome, javaExeName);
        writeLog("INFO", "Using custom Java home: %s", javaHome);

        // Verify the path exists
        DWORD attrib = GetFileAttributesA(javaPath);
        if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            char error[1024];
            snprintf(error, sizeof(error),
                     "Java not found at specified location:\n%s\n\nPlease check your --java-home path.",
                     javaPath);
            showMessage(hasConsole, "Java Not Found", error, MB_ICONERROR);
            free(javaHome);
            closeLog();
            return 1;
        }

        free(javaHome);
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
            closeLog();
            return 1;
        }
        writeLog("INFO", "Found Java in PATH: %s", javaPath);
    }

    // Build final command line
    char finalCmdLine[MAX_CMD_LEN];
    char timingProps[512];
    char aotArg[MAX_PATH + 50] = {0};
    char jarFilePath[MAX_PATH] = {0};

    // Measure time before JVM invocation
    long long beforeJVMInvokeMicros = getElapsedMicros();

    // Build timing system properties
    snprintf(timingProps, sizeof(timingProps),
             "-Djarrunner.start.micros=%lld -Djarrunner.beforejvm.micros=%lld",
             startTimeMicros, beforeJVMInvokeMicros);

    if (useConfig && config.javaArgs[0]) {
        // Config mode: build command from config
        writeLog("INFO", "Using config-based mode");

        // Extract JAR path for AOT (if using -jar)
        extractJarPath(config.javaArgs, jarFilePath, sizeof(jarFilePath));

        // Build AOT cache path if enabled
        if (enableAOT && jarFilePath[0]) {
            char aotCachePath[MAX_PATH];
            buildAOTCacheName(jarFilePath, aotCachePath, sizeof(aotCachePath));

            if (aotCachePath[0]) {
                // Clean up old AOT files
                cleanupOldAOTFiles(jarFilePath, aotCachePath);

                // Check if AOT cache exists
                DWORD attrib = GetFileAttributesA(aotCachePath);
                BOOL aotExists = (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));

                if (aotExists) {
                    snprintf(aotArg, sizeof(aotArg), "-XX:AOTCache=\"%s\"", aotCachePath);
                    writeLog("INFO", "Using existing AOT cache: %s", aotCachePath);
                } else {
                    snprintf(aotArg, sizeof(aotArg), "-XX:AOTCacheOutput=\"%s\"", aotCachePath);
                    writeLog("INFO", "Creating new AOT cache: %s", aotCachePath);
                }
            }
        }

        // Get command-line args (skip past exe name)
        char cmdLineArgs[MAX_CMD_LEN] = {0};
        char* argsStart = fullCmdLine;
        if (*argsStart == '"') {
            argsStart = strchr(argsStart + 1, '"');
            if (argsStart) argsStart += 2;
        } else {
            argsStart = strchr(argsStart, ' ');
            if (argsStart) argsStart++;
        }
        if (argsStart) {
            while (*argsStart == ' ') argsStart++;
            // Remove --java-home if present
            char tempArgs[MAX_CMD_LEN];
            strncpy(tempArgs, argsStart, sizeof(tempArgs) - 1);
            removeJavaHomeArg(tempArgs);
            // Remove --disable-aot/--enable-aot flags
            char* flag = strstr(tempArgs, "--disable-aot");
            if (flag) {
                char* end = flag + 13;
                if (*end == ' ') end++;
                memmove(flag, end, strlen(end) + 1);
            }
            flag = strstr(tempArgs, "--enable-aot");
            if (flag) {
                char* end = flag + 12;
                if (*end == ' ') end++;
                memmove(flag, end, strlen(end) + 1);
            }
            trim(tempArgs);
            if (tempArgs[0]) {
                strncpy(cmdLineArgs, tempArgs, sizeof(cmdLineArgs) - 1);
            }
        }

        // Build final command: java [timing] [vm.args] [aot] [java.args] [app.args] [cmdline-args]
        int pos = snprintf(finalCmdLine, sizeof(finalCmdLine), "\"%s\" %s", javaPath, timingProps);

        if (config.vmArgs[0]) {
            pos += snprintf(finalCmdLine + pos, sizeof(finalCmdLine) - pos, " %s", config.vmArgs);
        }

        if (aotArg[0]) {
            pos += snprintf(finalCmdLine + pos, sizeof(finalCmdLine) - pos, " %s", aotArg);
        }

        pos += snprintf(finalCmdLine + pos, sizeof(finalCmdLine) - pos, " %s", config.javaArgs);

        if (config.appArgs[0]) {
            pos += snprintf(finalCmdLine + pos, sizeof(finalCmdLine) - pos, " %s", config.appArgs);
        }

        if (cmdLineArgs[0]) {
            snprintf(finalCmdLine + pos, sizeof(finalCmdLine) - pos, " %s", cmdLineArgs);
        }

    } else {
        // Traditional mode: JAR as first argument
        writeLog("INFO", "Using traditional mode (no config file)");

        // Skip past the executable name in command line
        char* jarArgs = fullCmdLine;
        if (*jarArgs == '"') {
            jarArgs = strchr(jarArgs + 1, '"');
            if (jarArgs) jarArgs += 2;
        } else {
            jarArgs = strchr(jarArgs, ' ');
            if (jarArgs) jarArgs++;
        }

        // Trim leading spaces
        while (jarArgs && *jarArgs == ' ') jarArgs++;

        if (!jarArgs || !*jarArgs) {
            // Show diagnostic/help information
            char info[2048];
            snprintf(info, sizeof(info),
                     "Java Runner (jr) - Smart Java Launcher\n\n"
                     "Execution Context: %s\n"
                     "Java Executable: %s\n"
                     "Java Location: %s\n"
                     "Config File: %s (not found)\n\n"
                     "Usage:\n"
                     "  %s.exe <jar-file> [args...]\n"
                     "  %s.exe --create-config [jar-file]\n"
                     "  %s.exe --java-home=PATH <jar-file> [args...]\n\n"
                     "Examples:\n"
                     "  %s.exe myapp.jar\n"
                     "  %s.exe --create-config myapp.jar\n"
                     "  %s.exe --java-home=C:\\Java\\jdk21 myapp.jar --verbose",
                     hasConsole ? "Console (terminal/cmd)" : "GUI (double-clicked)",
                     javaExeName,
                     javaPath,
                     configPath,
                     exeBaseName, exeBaseName, exeBaseName,
                     exeBaseName, exeBaseName, exeBaseName);
            showMessage(hasConsole, "Java Runner - Help", info, MB_ICONINFORMATION);
            closeLog();
            return 1;
        }

        // Remove --java-home from args
        char tempArgs[MAX_CMD_LEN];
        strncpy(tempArgs, jarArgs, sizeof(tempArgs) - 1);
        removeJavaHomeArg(tempArgs);

        // Remove AOT flags
        char* flag = strstr(tempArgs, "--disable-aot");
        if (flag) {
            char* end = flag + 13;
            if (*end == ' ') end++;
            memmove(flag, end, strlen(end) + 1);
            trim(tempArgs);
        }
        flag = strstr(tempArgs, "--enable-aot");
        if (flag) {
            char* end = flag + 12;
            if (*end == ' ') end++;
            memmove(flag, end, strlen(end) + 1);
            trim(tempArgs);
        }

        // Extract JAR file path
        extractJarPath(tempArgs, jarFilePath, sizeof(jarFilePath));
        if (!jarFilePath[0]) {
            // Maybe it's just a direct JAR path (not -jar format)
            char* firstArg = tempArgs;
            if (*firstArg == '"') {
                firstArg++;
                char* endQuote = strchr(firstArg, '"');
                if (endQuote) {
                    size_t len = endQuote - firstArg;
                    if (len < sizeof(jarFilePath)) {
                        strncpy(jarFilePath, firstArg, len);
                        jarFilePath[len] = '\0';
                    }
                }
            } else {
                char* end = strchr(firstArg, ' ');
                if (!end) end = firstArg + strlen(firstArg);
                size_t len = end - firstArg;
                if (len < sizeof(jarFilePath)) {
                    strncpy(jarFilePath, firstArg, len);
                    jarFilePath[len] = '\0';
                }
            }
        }

        // Build AOT cache path if enabled
        if (enableAOT && jarFilePath[0]) {
            char aotCachePath[MAX_PATH];
            buildAOTCacheName(jarFilePath, aotCachePath, sizeof(aotCachePath));

            if (aotCachePath[0]) {
                // Clean up old AOT files
                cleanupOldAOTFiles(jarFilePath, aotCachePath);

                // Check if AOT cache exists
                DWORD attrib = GetFileAttributesA(aotCachePath);
                BOOL aotExists = (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));

                if (aotExists) {
                    snprintf(aotArg, sizeof(aotArg), "-XX:AOTCache=\"%s\"", aotCachePath);
                    writeLog("INFO", "Using existing AOT cache: %s", aotCachePath);
                } else {
                    snprintf(aotArg, sizeof(aotArg), "-XX:AOTCacheOutput=\"%s\"", aotCachePath);
                    writeLog("INFO", "Creating new AOT cache: %s", aotCachePath);
                }
            }
        }

        // Build final command: java [timing] [aot] -jar <remaining args>
        if (aotArg[0]) {
            snprintf(finalCmdLine, sizeof(finalCmdLine), "\"%s\" %s %s -jar %s",
                     javaPath, timingProps, aotArg, tempArgs);
        } else {
            snprintf(finalCmdLine, sizeof(finalCmdLine), "\"%s\" %s -jar %s",
                     javaPath, timingProps, tempArgs);
        }
    }

    writeLog("INFO", "Final command: %s", finalCmdLine);

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

        writeLog("INFO", "Java process started successfully (PID: %lu)", pi.dwProcessId);

        if (hasConsole) {
            // Console mode: Wait for Java process to complete
            WaitForSingleObject(pi.hProcess, INFINITE);

            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);

            writeLog("INFO", "Java process exited with code: %lu", exitCode);

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            closeLog();
            return exitCode;
        } else {
            // GUI mode: Launch and exit immediately
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            writeLog("INFO", "Launched in GUI mode, launcher exiting");
            closeLog();
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

    closeLog();
    return 1;
}
