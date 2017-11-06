#include <time.h>
#include <iomanip>
#include <sstream>
using namespace std;


extern "C" const char* strptime(const char* s, const char* f, struct tm* tm) {
    istringstream input(s);
    input.imbue(locale("en"));
    input >> get_time(tm, f);
    if (input.fail()) {
        return nullptr;
    }

    return s;
}