#include <unicode/ucol.h>
#include <unicode/ucasemap.h>

#ifdef __cplusplus
extern "C" {
#endif

UCollator* lc_ucol_open(const char* loc, UErrorCode* status);
void lc_ucol_setAttribute(UCollator* coll, UColAttribute attr, UColAttributeValue value, UErrorCode* status);
void lc_ucol_close(UCollator* coll);
UCollationResult lc_ucol_strcollUTF8(const UCollator* coll, const char* source, int32_t sourceLength, const char* target, int32_t targetLength, UErrorCode* status);
UCaseMap* lc_ucasemap_open(const char* locale, uint32_t options, UErrorCode* pErrorCode);
int32_t lc_ucasemap_utf8ToUpper(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode);
int32_t lc_ucasemap_utf8ToLower(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode);
void lc_ucasemap_close(UCaseMap* csm);

#ifdef __cplusplus
}
#endif
