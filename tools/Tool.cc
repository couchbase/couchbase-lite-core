//
//  Tool.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/31/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Tool.hh"
#include "Logging.hh"
#include "linenoise.h"
#include <stdio.h>

#if __APPLE__
#include <unistd.h>
#include <sys/ioctl.h>
#endif


static constexpr int kDefaultLineWidth = 100;


Tool* Tool::instance;


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

static bool tokenize(const char* input, deque<string> &args)
{
    string str(input);
    int start = 0;
    for( size_t i=0; i<str.length(); i++){
        char c = str[i];
        if( c == ' ' ) {
            if(i - start > 1) {
                args.push_back(str.substr(start, i - start));
            }
            
            while(str[i] == ' ') {
                i++;
            }
            
            start = i;
            i--;
        } else if(c == '\"' ) {
            i++;
            while( i < str.size() - 1 && str[i] != '\"' ){ i++; }
            if(str[i] != '\"') {
                return false;
            }
            
            args.push_back(str.substr(start + 1, i - start - 1));
            start = i;
        } else if(c == '\r' || c == '\n') {
            break;
        } else if(!isascii(c)) {
            start = i+1;
        }
    }
    
    int offset = 0;
    int carriageReturn = str.find("\r");
    int lineFeed = str.find("\n");
    if(carriageReturn > 0) {
        offset++;
    }
    
    if(lineFeed > 0) {
        offset++;
    }
    
    if(str[str.length() - offset - 1] == '\"') {
        offset++;
    }
    
    if(str.length() - start - offset > 1) {
        args.push_back(str.substr(start, str.length() - start - offset));
    }
    
    return true;
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

    linenoiseHistorySetMaxLen(100);

    _args.clear();
    _editPrompt = prompt;
    if (_colorMode && outputIsColor()) {
        _editPrompt.insert(0, ANSI_COLOR_PROMPT);
        _editPrompt += ANSI_COLOR_RESET;
    }

    while (true) {
        const char* line = linenoise(_editPrompt.c_str());
        // Returned line and lineLength include the trailing newline, unless user typed ^D.
        if (linenoiseKeyType() == 2) {
            // EOF on stdin; return false:
            cout << "\n";
            return false;
        } else if (line == nullptr) {
            // Error in linenoise
            fail("reading interactive input");
        } else if (linenoiseKeyType() == 0) {
            // Got a command!
            // Add line to history so user can recall it later:
            linenoiseHistoryAdd(line);

            // Tokenize the line (break into args):
            bool success = tokenize(line, _args);
            if (!success) {
                cout << "Error: Unclosed quote\n";
                continue;
            }
            
            // Return true unless there are no actual args:
            if (!_args.empty())
                return true;
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
        
        bool success = tokenize(line, _args);
        if (!success) {
            cout << "Error: Unclosed quote\n";
            continue;
        }

        // Return true unless there are no actual args:
        if (!_args.empty())
            return true;
        cout << "Please type a command, or Ctrl-D to exit.\n";
    }
}
