#if LITECORE_USES_ICU

#ifdef _MSC_VER
#include <icu.h>
#else
#include <unicode/ucol.h>
#include <unicode/ucasemap.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER

// ICU is a part of Windows since Windows 10 1709, no need to shim
// since it is not ultra hard versioned like its Linux counterpart

#define lc_ucol_open ucol_open
#define lc_ucol_setAttribute ucol_setAttribute
#define lc_ucol_close ucol_close
#define lc_ucol_strcollUTF8 ucol_strcollUTF8
#define lc_ucasemap_open ucasemap_open
#define lc_ucasemap_utf8ToUpper ucasemap_utf8ToUpper
#define lc_ucasemap_utf8ToLower ucasemap_utf8ToLower
#define lc_ucasemap_close ucasemap_close
#define lc_ucol_countAvailable ucol_countAvailable
#define lc_ucol_getAvailable ucol_getAvailable

#else

UCollator* lc_ucol_open(const char* loc, UErrorCode* status);
void lc_ucol_setAttribute(UCollator* coll, UColAttribute attr, UColAttributeValue value, UErrorCode* status);
void lc_ucol_close(UCollator* coll);
UCollationResult lc_ucol_strcollUTF8(const UCollator* coll, const char* source, int32_t sourceLength, const char* target, int32_t targetLength, UErrorCode* status);
UCaseMap* lc_ucasemap_open(const char* locale, uint32_t options, UErrorCode* pErrorCode);
int32_t lc_ucasemap_utf8ToUpper(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode);
int32_t lc_ucasemap_utf8ToLower(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode);
void lc_ucasemap_close(UCaseMap* csm);
int32_t lc_ucol_countAvailable(void);
const char* lc_ucol_getAvailable(int32_t localeIndex);

#endif

#ifdef __cplusplus
}
#endif

#endif