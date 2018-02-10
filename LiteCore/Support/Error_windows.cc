//
// Error_windows.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#ifdef _MSC_VER
#pragma comment(lib, "Dbghelp.lib")
#include <Windows.h>
#include <Dbghelp.h>
#include "Error.hh"
#include "asprintf.h"
#include <sstream>
using namespace std;

namespace litecore {
    /*static*/ string error::backtrace(unsigned skip) {
        #if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        void* stack[50];
        const auto process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        const auto captured = CaptureStackBackTrace(0, 50, stack, nullptr);
        const auto symbol = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO)+1023 * sizeof(TCHAR));
        symbol->MaxNameLen = 1024;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        DWORD displacement;
        const auto line = (IMAGEHLP_LINE64*)malloc(sizeof(IMAGEHLP_LINE64));
        line->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        
		stringstream out;
        for(unsigned i = skip + 1; i < captured - skip; i++) {
            const auto address = (DWORD64)stack[i];
            SymFromAddr(process, address, nullptr, symbol);
			char* cstr = nullptr;
            if(SymGetLineFromAddr64(process, address, &displacement, line)) {
				asprintf(&cstr, "\tat %s in %s: line: %lu: address: 0x%0llX\r\n", symbol->Name, line->FileName, line->LineNumber, symbol->Address);
            } else {
                asprintf(&cstr, "\tat %s, address 0x%0llX.\r\n", symbol->Name, symbol->Address);
            }

			out << cstr;
			free(cstr);
        }
        
        free(symbol);
        free(line);
        SymCleanup(process);

		return out.str();
		#else
		return "(stack trace unavailable on Windows Store)";
        #endif
    }
}

#endif
