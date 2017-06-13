#include <time.h>
#include <iomanip>
#include <sstream>
using namespace std;

extern "C" char* strptime(const char* s, const char* f, struct tm* tm) {
    istringstream input(s);
    input.imbue(locale(setlocale(LC_ALL, nullptr)));
    input >> get_time(tm, f);
    if (input.fail()) {
        return nullptr;
    }
    
    return (char*)(s + input.tellg());
}