//
//  Tool.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/31/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Tool.hh"
#include "Logging.hh"
#include <histedit.h>           // editline / libedit API
#include <stdio.h>

#if __APPLE__
#include <unistd.h>
#endif


Tool::~Tool() {
    if (_editLine) {
        if (_editHistory)
            history_end(_editHistory);
        if (_editTokenizer)
            tok_end(_editTokenizer);
        el_end(_editLine);
    }
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


const char* Tool::promptCallback(EditLine *e) {
    Tool *tool;
    el_get(e, EL_CLIENTDATA, &tool);
    return tool->_editPrompt.c_str();
}

bool Tool::readLine(const char *prompt) {
    if (!inputIsTerminal())
        return dumbReadLine(prompt);

    if (!_editLine) {
        // First-time initialization of editline:
        _editLine = el_init("cblite", stdin, stdout, stderr);   // FIX: Make name configurable
        el_set(_editLine, EL_SIGNAL, 1);
        el_set(_editLine, EL_EDITOR, "emacs");
        el_set(_editLine, EL_CLIENTDATA, this);
        el_set(_editLine, EL_PROMPT, &promptCallback);

        _editHistory = history_init();
        if (_editHistory) {
            HistEvent ev;
            history(_editHistory, &ev, H_SETSIZE, 100);        // Set history size
            el_set(_editLine, EL_HIST, &history, _editHistory);        // Attach history to line editor
        } else {
            Warn("command-line history could not be initialized");
        }

        _editTokenizer = tok_init(nullptr);
    }

    _args.clear();
    _editPrompt = prompt;
    if (_colorMode && outputIsColor()) {
        _editPrompt.insert(0, ANSI_COLOR_PROMPT);
        _editPrompt += ANSI_COLOR_RESET;
    }

    while (true) {
        int lineLength;
        const char *line = el_gets(_editLine, &lineLength);
        // Returned line and lineLength include the trailing newline, unless user typed ^D.
        if (lineLength < 0) {
            // Error in editline
            fail("reading interactive input");
        } else if (lineLength == 0) {
            // EOF on stdin; return false:
            cout << "\n";
            return false;
        } else if (lineLength > 1) {
            // Got a command!
            // Add line to history so user can recall it later:
            HistEvent ev;
            history(_editHistory, &ev, H_ENTER, line);

            // Tokenize the line (break into args):
            tok_reset(_editTokenizer);
            int argc;
            const char **argv;
            int eol = tok_line(_editTokenizer, el_line(_editLine), &argc, &argv, nullptr, nullptr);
            if (eol > 0) {
                cout << "Error: Unclosed quote\n";
                continue;
            }
            // Convert the args into C++ form:
            for(int i = 0; i < argc; ++i)
                _args.emplace_back(argv[i]);
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
        int argc;
        const char **argv;
        auto tokenizer = tok_init(nullptr);
        int eol = tok_str(tokenizer, line, &argc, &argv);
        if (eol > 0) {
            cout << "Error: Unclosed quote\n";
            continue;
        }
        // Convert the args into C++ form:
        for(int i = 0; i < argc; ++i)
            _args.emplace_back(argv[i]);
        // Return true unless there are no actual args:
        if (!_args.empty())
            return true;
        cout << "Please type a command, or Ctrl-D to exit.\n";
    }
}
