//
// Tokenizer.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
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

#include "Tokenizer.hh"
#include "Error.hh"
#include "fts3_tokenizer.h"
#include <mutex>

namespace litecore {
    using namespace std;


    // The design of the fts3_tokenizer API only allows there to be one module registered
    // (at least with the same source code); so make it a singleton.
    static sqlite3_tokenizer_module* sModule;
    static C4TokenizerFactory sFactory;


    class Cursor : public sqlite3_tokenizer_cursor {
    public:
        Cursor(C4TokenizerCursor* NONNULL c4cursor, slice input)
        :_c4cursor(c4cursor)
        ,_input(input)
        { }

        ~Cursor() {
            if (_c4cursor->methods->free)
                _c4cursor->methods->free(_c4cursor);
        }

        int next(const char **ppToken, int *pnBytes,
                 int *piStartOffset,
                 int *piEndOffset,
                 int *piPosition)
        {
            C4Slice normalizedToken, tokenRange;
            C4Error error = {};
            if (!_c4cursor->methods->next(_c4cursor, &normalizedToken, &tokenRange, &error))
                return error.code ? SQLITE_ERROR : SQLITE_DONE;
            *ppToken = (const char*)normalizedToken.buf;
            *pnBytes = int(normalizedToken.size);
            *piStartOffset = int((char*)tokenRange.buf - (char*)_input.buf);
            *piEndOffset = int(*piStartOffset + tokenRange.size);
            *piPosition = _pos++;
            return SQLITE_OK;
        }

    private:
        C4TokenizerCursor* _c4cursor;
        slice _input;
        int _pos {0};
    };


    class Tokenizer : public sqlite3_tokenizer {
    public:
        static void registerFactory(C4TokenizerFactory NONNULL);

        static void registerWithDatabase(sqlite3* NONNULL);

        Tokenizer(C4Tokenizer* NONNULL c4tok)
        :_c4tok(c4tok)
        { }

        ~Tokenizer() {
            if (_c4tok->methods->free)
                _c4tok->methods->free(_c4tok);
        }

        Cursor* open(slice text) {
            auto curs = _c4tok->methods->newCursor(_c4tok, text, nullptr);
            return curs ? new Cursor(curs, text) : nullptr;
        }

    private:
        C4Tokenizer* _c4tok;
    };


    static sqlite3_tokenizer_module* CreateModule() {
        auto module = new sqlite3_tokenizer_module;
        module->iVersion = 1;

        module->xCreate = [](int argc, const char *const*argv, sqlite3_tokenizer **outTok) {
            if (!sFactory)
                return SQLITE_ERROR;
            const C4IndexOptions *options = nullptr;
            if (argc > 0 && strncmp(argv[0], "options=", 8) == 0) {
                char *end;
                options = (const C4IndexOptions*) strtol(argv[0] + 8, &end, 16);
                if (*end != '\0' || options == 0)
                    return SQLITE_ERROR;
            }
            C4Tokenizer *c4Tok = sFactory(options);
            if (!c4Tok)
                return SQLITE_ERROR;
            *outTok = new Tokenizer(c4Tok);
            return SQLITE_OK;
        };

        module->xDestroy = [](sqlite3_tokenizer *t) {
            delete (Tokenizer*)t;
            return SQLITE_OK;
        };

        module->xOpen = [](sqlite3_tokenizer *pTokenizer,       /* Tokenizer object */
                            const char *pInput, int nBytes,      /* Input buffer */
                            sqlite3_tokenizer_cursor **ppCursor) /* OUT: Created tokenizer cursor */
        {
            if (nBytes < 0)
                nBytes = (int)strlen(pInput);
            *ppCursor = ((Tokenizer*)pTokenizer)->open({pInput, (size_t)nBytes});
            return *ppCursor ? SQLITE_OK : SQLITE_ERROR;
        };

        module->xClose = [](sqlite3_tokenizer_cursor *cursor) {
            delete ((Cursor*)cursor);
            return SQLITE_OK;
        };

        module->xNext = [](sqlite3_tokenizer_cursor *pCursor,   /* Tokenizer cursor */
                            const char **ppToken, int *pnBytes,  /* OUT: Normalized text for token */
                            int *piStartOffset,  /* OUT: Byte offset of token in input buffer */
                            int *piEndOffset,    /* OUT: Byte offset of end of token in input buffer */
                            int *piPosition)     /* OUT: Number of tokens returned before this one */
        {
            return ((Cursor*)pCursor)->next(ppToken, pnBytes,
                                            piStartOffset, piEndOffset, piPosition);
        };

        module->xLanguageid = [](sqlite3_tokenizer_cursor *cursor, int iLangid) {
            return SQLITE_OK;   // ignored
        };

        return module;
    }


#pragma mark - API:


    void RegisterC4TokenizerFactory(C4TokenizerFactory factory) {
        sFactory = factory;
    }


    int InstallC4Tokenizer(sqlite3 *db) {
        static once_flag once;
        call_once(once, []{ sModule = CreateModule(); });

        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db, "SELECT fts3_tokenizer(?, ?)", -1, &stmt, 0);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, kC4TokenizerName, -1, SQLITE_STATIC);
            sqlite3_bind_blob(stmt, 2, &sModule, sizeof(sModule), SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            rc = sqlite3_finalize(stmt);
        }
        return rc;
    }


    bool HaveC4Tokenizer() {
        return sFactory != nullptr;
    }

}

