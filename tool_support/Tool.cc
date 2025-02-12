//
// Tool.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Tool.hh"
#include "Logging.hh"
#include "linenoise.h"
#include "utf8.h"
#include <charconv>
#include <cstdio>
#include <fstream>
#include <regex>

#if !defined(_MSC_VER)
#include <unistd.h>
#include <sys/ioctl.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <conio.h>
#define isatty _isatty
#define STDIN_FILENO _fileno(stdin)
#define STDOUT_FILENO _fileno(stdout)
#endif

using namespace std;
using namespace fleece;
using namespace litecore;

static constexpr int kDefaultLineWidth = 100;

static constexpr const char *kHistoryFilePath = "~/.cblite_history";


Tool* Tool::instance;

Tool::Tool(const char* name)
:_name(name)
{
    if(!instance) {
        instance = this;
    }
}

Tool::~Tool() {
    if (this == instance) {
        instance = nullptr;
    }
}


#ifdef _MSC_VER
    typedef LONG NTSTATUS, *PNTSTATUS;
    #define STATUS_SUCCESS (0x00000000)

    typedef NTSTATUS (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

    RTL_OSVERSIONINFOW GetRealOSVersion() {
        HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
        if (hMod) {
            RtlGetVersionPtr fxPtr = (RtlGetVersionPtr)::GetProcAddress(hMod, "RtlGetVersion");
            if (fxPtr != nullptr) {
                RTL_OSVERSIONINFOW rovi = { 0 };
                rovi.dwOSVersionInfoSize = sizeof(rovi);
                if ( STATUS_SUCCESS == fxPtr(&rovi) ) {
                    return rovi;
                }
            }
        }
        RTL_OSVERSIONINFOW rovi = { 0 };
        return rovi;
    }
#endif


static bool sOutputIsColor = false;

void Tool::enableColor() {
    if (getenv("CLICOLOR_FORCE")) {
        sOutputIsColor = true;
        return;
    }
    
    const char *term = getenv("TERM");
    if(isatty(STDOUT_FILENO)
            && term != nullptr
            && (strstr(term,"ANSI") || strstr(term,"ansi") || strstr(term,"color"))) {
        sOutputIsColor = true;
        return;
    }

#ifdef _MSC_VER
    #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
    // Sick of this being missing for whatever reason
    #define ENABLE_VIRTUAL_TERMINAL_PROCESSING  0x0004
    #endif

    sOutputIsColor = GetRealOSVersion().dwMajorVersion >= 10;
    if(sOutputIsColor) {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD consoleMode;
        if(GetConsoleMode(hConsole, &consoleMode)) {
            SetConsoleMode(hConsole, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif

}

string Tool::ansi(const char *command) {
    if (sOutputIsColor)
        return stringprintf("\033[%sm", command);
    else
        return "";
}


int Tool::terminalWidth() {
#if __APPLE__
    struct ttysize ts;
    if (ioctl(0, TIOCGSIZE, &ts) == 0 && ts.ts_cols > 0)
        return ts.ts_cols;
#endif
    return kDefaultLineWidth;
}


static Tool* sLineReader = nullptr;     // The instance that's currently calling linenoise()


bool Tool::readLine(const char *cPrompt) {
    initReadLine();

    string prompt = cPrompt;
    if (sOutputIsColor)
        prompt = ansiBold() + prompt + ansiReset();

    while (true) {
        sLineReader = this;
        char* line = linenoise(prompt.c_str());
        sLineReader = nullptr;
        if (line == nullptr) {
            // EOF (Ctrl-D)
            return false;
        } else if (*line == 0) {
            // No command was entered, so go round again:
            cout << "Please type a command, or Ctrl-D to exit.\n";
        } else if (*line) {
            // Got a command!
            // Add line to history so user can recall it later:
            linenoiseHistoryAdd(line);
#ifndef _MSC_VER
            linenoiseHistorySave(fixedUpPath(kHistoryFilePath).c_str());
#endif
            _argTokenizer.reset(line);
            linenoiseFree(line);
            return true;
        }
    }
}


// Initialize linenoise library:
void Tool::initReadLine() {
    static bool sLineNoiseInitialized = false;
    if (sLineNoiseInitialized)
        return;

#ifdef __APPLE__
    // Prevent linenoise from trying to use ANSI escapes in the Xcode console on macOS,
    // which is a TTY but does not set $TERM. For some reason linenoise thinks a missing $TERM
    // indicates an ANSI-compatible terminal (isUnsupportedTerm() in linenoise.c.)
    // So if $TERM is not set, set it to "dumb", which linenoise does understand.
    if (isatty(STDIN_FILENO) && getenv("TERM") == nullptr)
        setenv("TERM", "dumb", false);
#endif

    // Enable UTF-8:
    linenoiseSetEncodingFunctions(linenoiseUtf8PrevCharLen, linenoiseUtf8NextCharLen,
                                  linenoiseUtf8ReadCode);

    // Install a command-completion callback that dispatches to the current Tool instance:
    linenoiseSetCompletionCallback([](const char *line, linenoiseCompletions *lc) {
        if (sLineReader) {
            ArgumentTokenizer tokenizer;
            tokenizer.reset(line);
            if (tokenizer.hasArgument()) {
                sLineReader->addLineCompletions(tokenizer, [&](const string &completion) {
                    linenoiseAddCompletion(lc, completion.c_str());
                });
            }
        }
    });

    // Initialize history, reloading from a saved file:
    linenoiseHistorySetMaxLen(100);
#ifndef _MSC_VER
    linenoiseHistoryLoad(fixedUpPath(kHistoryFilePath).c_str());
#endif

    sLineNoiseInitialized = true;
}


string Tool::readPassword(const char *prompt) {
#if defined(_MSC_VER)
    cout << prompt;
    string pass;
    const char BACKSPACE = 8;
    const char CARRIAGE_RETURN = 13;
    const char CTRL_C = 3;
    int next;
    while((next = _getch()) != CARRIAGE_RETURN) {
        if(next == CTRL_C) {
            pass.clear();
            break;
        }

        if(next == BACKSPACE) {
            pass.resize(pass.length() - 1);
            continue;
        }

        if(next < ' ') {
            // Disregard other non-printables
            continue;
        }

        pass += static_cast<char>(next);
    }

    cout << endl;
    return pass;
#else
    char *cpass = getpass(prompt);
    string password = cpass;
    memset(cpass, 0, strlen(cpass) + 1); // overwrite password at known static location
    return password;
#endif
}


alloc_slice Tool::readFile(const string &path, bool mustExist) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file) {
        if (fseek(file, 0, SEEK_END) == 0) {
            auto size = ftell(file);
            fseek(file, 0, SEEK_SET);
            alloc_slice data(size);
            if (fread((char*)data.buf, 1, size, file) == size) {
                fclose(file);
                return data;
            }
        }
    }
    int err = errno;
    if (file)
        fclose(file);
    if (!mustExist && err == ENOENT)
        return nullslice;
    fail("Couldn't read file " + path + ": " + strerror(err));
}


void Tool::writeFile(slice data, const string& path, const char *mode) {
    FILE* file = fopen(path.c_str(), mode);
    if (file) {
        if (fwrite(data.buf, 1, data.size, file) == data.size) {
            fclose(file);
            return;
        }
    }
    int err = errno;
    if (file)
        fclose(file);
    fail("Couldn't write file " + path + ": " + strerror(err));
}
