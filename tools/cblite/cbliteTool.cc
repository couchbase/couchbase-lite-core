//
// cbliteTool.cc
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

#include "cbliteTool.hh"


int main(int argc, const char * argv[]) {
    CBLiteTool tool;
    return tool.main(argc, argv);
}


void CBLiteTool::usage() {
    cerr <<
    ansiBold() << "cblite: Couchbase Lite / LiteCore database multi-tool\n" << ansiReset() <<
    "Usage: cblite help " << it("[SUBCOMMAND]") << "\n"
    "       cblite cat " << it("[FLAGS] DBPATH DOCID [DOCID...]") << "\n"
    "       cblite cp " << it("[FLAGS] SOURCE DESTINATION") << "\n"
    "       cblite file " << it("DBPATH") << "\n"
    "       cblite ls " << it("[FLAGS] DBPATH [PATTERN]") << "\n"
    "       cblite query " << it("[FLAGS] DBPATH JSONQUERY") << "\n"
    "       cblite revs " << it("DBPATH DOCID") << "\n"
    "       cblite serve " << it("DBPATH") << "\n"
    "       cblite sql " << it("DBPATH QUERY") << "\n"
    "       cblite " << it("DBPATH") << "   (interactive shell)\n"
    "           The shell accepts the same commands listed above, but without the\n"
    "           'cblite' and DBPATH parameters. For example, 'ls -l'.\n"
    "   For information about parameters, run `cblite help`.\n"
    ;
}


void CBLiteTool::writeUsageCommand(const char *cmd, bool hasFlags, const char *otherArgs) {
    cerr << ansiBold();
    if (!_interactive)
        cerr << "cblite ";
    cerr << cmd << ' ' << ansiItalic();
    if (hasFlags)
        cerr << "[FLAGS]" << ' ';
    if (!_interactive)
        cerr << "DBPATH ";
    cerr << otherArgs << ansiReset() << "\n";
}


int CBLiteTool::run() {
    c4log_setCallbackLevel(kC4LogWarning);
    clearFlags();
    if (argCount() == 0) {
        cerr << ansiBold()
             << "cblite: Couchbase Lite / LiteCore database multi-tool\n" << ansiReset() 
             << "Missing subcommand or database path.\n"
             << "For a list of subcommands, run " << ansiBold() << "cblite help" << ansiReset() << ".\n"
             << "To start the interactive mode, run "
             << ansiBold() << "cblite " << ansiItalic() << "DBPATH" << ansiReset() << '\n';
        fail();
    }
    string cmd = nextArg("subcommand or database path");
    if (hasSuffix(cmd, kC4DatabaseFilenameExtension)) {
        endOfArgs();
        openDatabase(cmd);
        runInteractively();
    } else {
        _currentCommand = cmd;
        if (!processFlag(cmd, kSubcommands))
            failMisuse(format("Unknown subcommand '%s'", cmd.c_str()));
    }
    return 0;
}


void CBLiteTool::openDatabase(string path) {
    C4DatabaseConfig config = {_dbFlags};
    C4Error err;
    _db = c4db_open(c4str(path), &config, &err);
    if (!_db)
        fail(format("Couldn't open database %s", path.c_str()), err);
}


void CBLiteTool::openDatabaseFromNextArg() {
    if (!_db)
        openDatabase(nextArg("database path"));
}


#pragma mark - INTERACTIVE MODE:


void CBLiteTool::shell() {
    // Read params:
    openDatabaseFromNextArg();
    endOfArgs();
    runInteractively();
}


void CBLiteTool::runInteractively() {
    _interactive = true;
    cout << "Opened database " << alloc_slice(c4db_getPath(_db)) << '\n';

    while(true) {
        try {
            if (!readLine("(cblite) "))
                return;
            string cmd = nextArg("subcommand");
            clearFlags();
            _currentCommand = cmd;
            if (!processFlag(cmd, kInteractiveSubcommands))
                cerr << format("Unknown subcommand '%s'; type 'help' for a list of commands.\n",
                               cmd.c_str());
        } catch (const fail_error &) {
            // subcommand failed (error message was already printed); continue
        }
    }
}


void CBLiteTool::helpCommand() {
    if (argCount() > 0) {
        _showHelp = true; // forces command to show help and return
        string cmd = nextArg("subcommand");
        if (!processFlag(cmd, kInteractiveSubcommands))
            cerr << format("Unknown subcommand '%s'\n", cmd.c_str());
    } else {
        catUsage();
        cpUsage();
        fileUsage();
        listUsage();
        queryUsage();
        revsUsage();
        sqlUsage();
        if (_interactive)
            cerr << ansiBold() << "help " << it("[COMMAND]") << ansiReset() << '\n'
            << ansiBold() << "quit" << ansiReset() << "  (or Ctrl-D)\n";
        else {
            cerr <<
            ansiBold() << "cblite help [SUBCOMMAND]\n" << ansiReset() <<
            "  Displays help for a command, or for all commands.\n" <<
            ansiBold() << "cblite DBPATH\n" << ansiReset() <<
            "  Starts an interactive shell where you can run multiple commands on the same database.\n";
        }
    }
}


void CBLiteTool::quitCommand() {
    exit(0);
}


#pragma mark - FLAGS & SUBCOMMANDS:


const Tool::FlagSpec CBLiteTool::kSubcommands[] = {
    {"cat",     (FlagHandler)&CBLiteTool::catDocs},
    {"cp",      (FlagHandler)&CBLiteTool::copyDatabase},
    {"file",    (FlagHandler)&CBLiteTool::fileInfo},
    {"help",    (FlagHandler)&CBLiteTool::helpCommand},
    {"ls",      (FlagHandler)&CBLiteTool::listDocsCommand},
    {"query",   (FlagHandler)&CBLiteTool::queryDatabase},
    {"revs",    (FlagHandler)&CBLiteTool::revsInfo},
    {"serve",   (FlagHandler)&CBLiteTool::serve},
    {"sql",     (FlagHandler)&CBLiteTool::sqlQuery},

    {"shell",   (FlagHandler)&CBLiteTool::shell},
    {nullptr, nullptr}
};

const Tool::FlagSpec CBLiteTool::kInteractiveSubcommands[] = {
    {"cat",     (FlagHandler)&CBLiteTool::catDocs},
    {"cp",      (FlagHandler)&CBLiteTool::copyDatabase},
    {"push",    (FlagHandler)&CBLiteTool::copyDatabase},
    {"export",  (FlagHandler)&CBLiteTool::copyDatabase},
    {"pull",    (FlagHandler)&CBLiteTool::copyDatabaseReversed},
    {"import",  (FlagHandler)&CBLiteTool::copyDatabaseReversed},
    {"file",    (FlagHandler)&CBLiteTool::fileInfo},
    {"help",    (FlagHandler)&CBLiteTool::helpCommand},
    {"ls",      (FlagHandler)&CBLiteTool::listDocsCommand},
    {"query",   (FlagHandler)&CBLiteTool::queryDatabase},
    {"revs",    (FlagHandler)&CBLiteTool::revsInfo},
    {"sql",     (FlagHandler)&CBLiteTool::sqlQuery},

    {"quit",    (FlagHandler)&CBLiteTool::quitCommand},
    {nullptr, nullptr}
};

const Tool::FlagSpec CBLiteTool::kQueryFlags[] = {
    {"--offset", (FlagHandler)&CBLiteTool::offsetFlag},
    {"--limit",  (FlagHandler)&CBLiteTool::limitFlag},
    {"--help",   (FlagHandler)&CBLiteTool::helpFlag},
    {nullptr, nullptr}
};

const Tool::FlagSpec CBLiteTool::kListFlags[] = {
    {"--offset", (FlagHandler)&CBLiteTool::offsetFlag},
    {"--limit",  (FlagHandler)&CBLiteTool::limitFlag},
    {"-l",       (FlagHandler)&CBLiteTool::longListFlag},
    {"--body",   (FlagHandler)&CBLiteTool::bodyFlag},
    {"--pretty", (FlagHandler)&CBLiteTool::prettyFlag},
    {"--raw",    (FlagHandler)&CBLiteTool::rawFlag},
    {"--json5",  (FlagHandler)&CBLiteTool::json5Flag},
    {"--desc",   (FlagHandler)&CBLiteTool::descFlag},
    {"--seq",    (FlagHandler)&CBLiteTool::seqFlag},
    {"--del",    (FlagHandler)&CBLiteTool::delFlag},
    {"--conf",   (FlagHandler)&CBLiteTool::confFlag},
    {"--help",   (FlagHandler)&CBLiteTool::helpFlag},
    {nullptr, nullptr}
};

const Tool::FlagSpec CBLiteTool::kCatFlags[] = {
    {"--pretty", (FlagHandler)&CBLiteTool::prettyFlag},
    {"--raw",    (FlagHandler)&CBLiteTool::rawFlag},
    {"--json5",  (FlagHandler)&CBLiteTool::json5Flag},
    {"--key",    (FlagHandler)&CBLiteTool::keyFlag},
    {"--rev",    (FlagHandler)&CBLiteTool::revIDFlag},
    {nullptr, nullptr}
};

const Tool::FlagSpec CBLiteTool::kCpFlags[] = {
    {"--limit",     (FlagHandler)&CBLiteTool::limitFlag},
    {"--existing",  (FlagHandler)&CBLiteTool::existingFlag},
    {"-x",          (FlagHandler)&CBLiteTool::existingFlag},
    {"--jsonid",    (FlagHandler)&CBLiteTool::jsonIDFlag},
    {"--careful",   (FlagHandler)&CBLiteTool::carefulFlag},
    {"--verbose",   (FlagHandler)&CBLiteTool::verboseFlag},
    {"-v",          (FlagHandler)&CBLiteTool::verboseFlag},
    {nullptr, nullptr}
};

const Tool::FlagSpec CBLiteTool::kServeFlags[] = {
    {"--replicate", (FlagHandler)&CBLiteTool::replicateFlag},
    {"--readonly",  (FlagHandler)&CBLiteTool::readonlyFlag},
    {"--port",      (FlagHandler)&CBLiteTool::portFlag},
    {"--verbose",   (FlagHandler)&CBLiteTool::verboseFlag},
    {"-v",          (FlagHandler)&CBLiteTool::verboseFlag},
    {nullptr, nullptr}
};

