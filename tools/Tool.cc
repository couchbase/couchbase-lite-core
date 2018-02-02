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
#include <cstdio>
#include <regex>

#if !defined(_MSC_VER)
#include <unistd.h>
#include <sys/ioctl.h>
#else
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO _fileno(stdin)
#define STDOUT_FILENO _fileno(stdout)
#endif


static constexpr int kDefaultLineWidth = 100;


Tool* Tool::instance;

Tool::Tool() {
    if(!instance) {
        instance = this;
    }
    
    linenoiseHistorySetMaxLen(100);
}

Tool::~Tool() {
    linenoiseHistoryFree();
    if (this == instance)
        instance = nullptr;
}


static bool inputIsTerminal() {
    return isatty(STDIN_FILENO) && getenv("TERM") != nullptr;
}

static bool outputIsColor() {
    static int sColor = -1;
    if (sColor < 0) {
        const char *term = getenv("TERM");
        sColor = isatty(STDOUT_FILENO)
            && term != nullptr
            && (strstr(term,"ANSI") || strstr(term,"ansi") || strstr(term,"color"));
    }
    return sColor;
}

// See <https://en.wikipedia.org/wiki/ANSI_escape_code#CSI_codes>
#define ANSI_COLOR_ESC(STR) "\033[" STR "m"
#define ANSI_COLOR_PROMPT   ANSI_COLOR_ESC("1")     // Bold
#define ANSI_COLOR_RESET    ANSI_COLOR_ESC("0")


string Tool::ansi(const char *command) {
    if (outputIsColor())
        return format("\033[%sm", command);
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

bool Tool::readLine(const char *prompt) {
    if (!inputIsTerminal())
        return dumbReadLine(prompt);

    _args.clear();
    _editPrompt = prompt;
    if (_colorMode && outputIsColor()) {
        _editPrompt.insert(0, ANSI_COLOR_PROMPT);
        _editPrompt += ANSI_COLOR_RESET;
    }

    while (true) {
        char* line = linenoise(_editPrompt.c_str());
        // Returned line and lineLength include the trailing newline, unless user typed ^D.
        if (line != nullptr) {
            // Got a command!
            // Add line to history so user can recall it later:
            linenoiseHistoryAdd(line);

            // Tokenize the line (break into args):
            bool success = _argTokenizer.tokenize(line, _args);
            if (!success) {
                cout << "Error: Unclosed quote or incomplete escape" << endl;
                continue;
            }
            
            // Return true unless there are no actual args:
            if (!_args.empty())
                return true;
        } else if(linenoiseKeyType() == 2) {
            cout << endl;
            return false;
        }
        
        // No command was entered, so go round again:
        cout << "Please type a command, or Ctrl-D to exit.\n";
    }
}


bool Tool::dumbReadLine(const char *prompt) {
    _args.clear();
    char inputBuffer[5000];
    while (true) {
        fputs(prompt, stdout);
        char* line = fgets(inputBuffer, sizeof(inputBuffer), stdin);
        if (!line) {
            cout << '\n';
            return false;
        }
        
        bool success = _argTokenizer.tokenize(line, _args);
        if (!success) {
            cout << "Error: Unclosed quote or incomplete escape" << endl;
            continue;
        }

        // Return true unless there are no actual args:
        if (!_args.empty())
            return true;
        cout << "Please type a command, or Ctrl-D to exit.\n";
    }
}
