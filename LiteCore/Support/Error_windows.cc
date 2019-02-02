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
#include "Error.hh"
#include <Windows.h>
#include <string>
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#include <DbgHelp.h>
#include "asprintf.h"
#include <sstream>
#include <Psapi.h>
#include <vector>
#include <algorithm>
#include <iterator>
#include <mutex>
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "psapi.lib")
using namespace std;

struct module_data {
	string image_name;
	string module_name;
	void* base_address;
	DWORD load_size;
};

namespace litecore {
    class get_mod_info {
		HANDLE process;
		static const int buffer_length = 4096;
	public:
		get_mod_info(const HANDLE h) : process(h) {}

		module_data operator()(HMODULE module) const {
			module_data ret;
			char temp[buffer_length];
			MODULEINFO mi;

			GetModuleInformation(process, module, &mi, sizeof(mi));
			ret.base_address = mi.lpBaseOfDll;
			ret.load_size = mi.SizeOfImage;

			GetModuleFileNameExA(process, module, temp, sizeof(temp));
			ret.image_name = temp;
			GetModuleBaseNameA(process, module, temp, sizeof(temp));
			ret.module_name = temp;
			SymLoadModule64(process, nullptr, ret.image_name.c_str(), ret.module_name.c_str(), (DWORD64)ret.base_address, ret.load_size);
			return ret;
		}
	};

    static mutex sStackWalker;

    /*static*/ string error::backtrace(unsigned skip) {
        lock_guard<mutex> lock(sStackWalker);
         void* stack[50];
		vector<module_data> modules;
		vector<HMODULE> module_handles(1);
        const auto process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
		DWORD symOptions = SymGetOptions();
		symOptions |= SYMOPT_LOAD_LINES | SYMOPT_UNDNAME;
		SymSetOptions(symOptions);
		DWORD cbNeeded;
		EnumProcessModules(process, &module_handles[0], module_handles.size() * sizeof(HMODULE), &cbNeeded);
		module_handles.resize(cbNeeded / sizeof(HMODULE));
		EnumProcessModules(process, &module_handles[0], module_handles.size() * sizeof(HMODULE), &cbNeeded);
		transform(module_handles.begin(), module_handles.end(), back_inserter(modules), get_mod_info(process));

        const auto captured = CaptureStackBackTrace(0, 50, stack, nullptr);
        const auto symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO)+256 * sizeof(TCHAR), 1);
        symbol->MaxNameLen = 255;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        DWORD displacement;
        const auto line = (IMAGEHLP_LINE64*)calloc(sizeof(IMAGEHLP_LINE64), 1);
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
    }
}
#else
using namespace std;
namespace litecore {
    /*static*/ string error::backtrace(unsigned skip) {
		return "(stack trace unavailable on Windows Store)";
    }
}
#endif

#endif
