//
//  Error_windows.cc
//  LiteCore
//
//  Created by Jim Borden on 2016/12/22.
//  Copyright c 2016 Couchbase. All rights reserved.
//

#ifdef _MSC_VER
#include <Windows.h>
#include <Dbghelp.h>
#include "Error.hh"
#include "Logging.hh"

namespace litecore {
    /*static*/ void error::logBacktrace(unsigned skip) {
        #if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        void* stack[50];
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, NULL, TRUE);
        DWORD captured = CaptureStackBackTrace(0, 50, stack, NULL);
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO)+1023 * sizeof(TCHAR));
        symbol->MaxNameLen = 1024;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        DWORD displacement;
        IMAGEHLP_LINE64 *line = (IMAGEHLP_LINE64*)malloc(sizeof(IMAGEHLP_LINE64));
        line->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        
        for(unsigned i = skip + 1; i < captured - skip; i++) {
            DWORD64 address = (DWORD64)stack[i];
            SymFromAddr(process, address, NULL, symbol);
            if(SymGetLineFromAddr64(process, address, &displacement, line)) {
                WarnError("\tat %s in %s: line: %lu: address: 0x%0X\n", symbol->Name, line->FileName, line->LineNumber, symbol->Address);
            } else {
                WarnError("\tat %s, address 0x%0X.\n", symbol->Name, symbol->Address);
            }
        }
        
        free(symbol);
        free(line);
        SymCleanup(process);
        #endif
    }
}

#endif
