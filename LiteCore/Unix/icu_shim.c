/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * Modified for use on Linux Desktop by Jim Borden <jim.borden@couchbase.com>
 */

#include "icu_shim.h"
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef CBL_USE_ICU_SHIM
#define LOCAL_INLINE

// This is not meant to be absolutely foolproof, but only to cover
// the triples we support on Linux
#if defined(__x86_64__)
#define ARCH_FOLDER "x86_64-linux-gnu"
#elif defined(__aarch64__)
#define ARCH_FOLDER "aarch64-linux-gnu"
#elif defined(__ARM_ARCH_7A__)
#define ARCH_FOLDER "arm-linux-gnueabihf"
#endif


#define ICUDATA_VERSION_MIN_LENGTH 2
#define ICUDATA_VERSION_MAX_LENGTH 3
#define ICUDATA_VERSION_MIN        44

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static char icudata_version[ICUDATA_VERSION_MAX_LENGTH + 1];

static void* handle_i18n = NULL;
static void* handle_common = NULL;
static void* syms[11];

/* The ICU data library from the ICU package is named libicudata.so.## */
static int filter(const struct dirent* dirp) {
  const char* name = dirp->d_name;
  const int len = strlen(name);

  // Valid length of the filename 'libicudata.so.##'
  if (len < 14 + ICUDATA_VERSION_MIN_LENGTH ||
      len > 14 + ICUDATA_VERSION_MAX_LENGTH) {
    return 0;
  }

  return !strncmp(name, "libicudata", 10);
}

static void init_icudata_version(void) {
  memset(icudata_version, 0, ICUDATA_VERSION_MAX_LENGTH + 1);
  memset(syms, 0, sizeof(syms));

  struct dirent** namelist = NULL;
  const char* icuDir = getenv("CBL_ICU_LOCATION") ?: "/usr/lib/" ARCH_FOLDER;
  int n = scandir(icuDir, &namelist, &filter, alphasort);
  int max_version = -1;
  while (n-- > 0) {
    int multiplier = 1;
    int ver = 0;
    char* name = namelist[n]->d_name;
    const int len = strlen(name);
    const char* verp = &name[len - 1];

    while(isdigit(*verp)) {
        ver += (*verp - '0') * multiplier;
        verp--;
        multiplier *= 10;
    }

    // We prefer the latest version available.
    if (ver > max_version) {
      max_version = ver;
      icudata_version[0] = '_';
      strcpy(icudata_version + 1, verp + 1);
    }

    free(namelist[n]);
  }
  free(namelist);

  if (max_version == -1 || max_version < ICUDATA_VERSION_MIN) {
    fprintf(stderr, "Cannot locate ICU data file at %s\n", icuDir);
    return;
  }

  char buffer[128];
  strcpy(buffer, "libicui18n.so.");
  strcat(buffer, icudata_version + 1);
  handle_i18n = dlopen(buffer, RTLD_LAZY);

  strcpy(buffer, "libicuuc.so.");
  strcat(buffer, icudata_version + 1);
  handle_common = dlopen(buffer, RTLD_LAZY);

  if (!handle_i18n || !handle_common) {
    fprintf(stderr, "Cannot open ICU libraries.\n");
    return;
  }

    printf("Found ICU libraries for version %d\n", max_version);

    strcpy(buffer, "ucol_open");
    strcat(buffer, icudata_version);
    syms[0] = dlsym(handle_i18n, buffer);

    strcpy(buffer, "ucol_setAttribute");
    strcat(buffer, icudata_version);
    syms[1] = dlsym(handle_i18n, buffer);

    strcpy(buffer, "ucol_strcollUTF8");
    strcat(buffer, icudata_version);
    syms[2] = dlsym(handle_i18n, buffer);

    strcpy(buffer, "ucol_close");
    strcat(buffer, icudata_version);
    syms[3] = dlsym(handle_i18n, buffer);

    strcpy(buffer, "ucol_strcoll");
    strcat(buffer, icudata_version);
    syms[4] = dlsym(handle_i18n, buffer);

    strcpy(buffer, "ucasemap_open");
    strcat(buffer, icudata_version);
    syms[5] = dlsym(handle_common, buffer);

    strcpy(buffer, "ucasemap_close");
    strcat(buffer, icudata_version);
    syms[6] = dlsym(handle_common, buffer);

    strcpy(buffer, "ucasemap_utf8ToLower");
    strcat(buffer, icudata_version);
    syms[7] = dlsym(handle_common, buffer);

    strcpy(buffer, "ucasemap_utf8ToUpper");
    strcat(buffer, icudata_version);
    syms[8] = dlsym(handle_common, buffer);

    strcpy(buffer, "uiter_setUTF8");
    strcat(buffer, icudata_version);
    syms[9] = dlsym(handle_common, buffer);

    strcpy(buffer, "ucol_strcollIter");
    strcat(buffer, icudata_version);
    syms[10] = dlsym(handle_i18n, buffer);
}
#else
#define LOCAL_INLINE inline
#endif

LOCAL_INLINE
UCollator* lc_ucol_open(const char* loc, UErrorCode* status) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  UCollator* (*ptr)(const char*, UErrorCode*);
  if (syms[0] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return (UCollator*)0;
  }
  ptr = (UCollator*(*)(const char*, UErrorCode*))syms[0];
  return ptr(loc, status);
  #else
  return ucol_open(loc, status);
  #endif
}

LOCAL_INLINE
void lc_ucol_setAttribute(UCollator* coll, UColAttribute attr, UColAttributeValue value, UErrorCode* status) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCollator*, UColAttribute, UColAttributeValue, UErrorCode*);
  if (syms[1] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return;
  }
  ptr = (void(*)(UCollator*, UColAttribute, UColAttributeValue, UErrorCode*))syms[1];
  ptr(coll, attr, value, status);
  #else
  ucol_setAttribute(coll, attr, value, status);
  #endif
}

LOCAL_INLINE
UCollationResult lc_ucol_strcollUTF8(const UCollator* coll, const char* source, int32_t sourceLength, const char* target, int32_t targetLength, UErrorCode* status) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  UCollationResult (*ptr)(const UCollator*, const char*, int32_t, const char*, int32_t, UErrorCode*);
  if (syms[2] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return (UCollationResult)0;
  }
  ptr = (UCollationResult(*)(const UCollator*, const char*, int32_t, const char*, int32_t, UErrorCode*))syms[2];
  return ptr(coll, source, sourceLength, target, targetLength, status);
  #else
  return ucol_strcollUTF8(coll, source, sourceLength, target, targetLength, status);
  #endif
}

LOCAL_INLINE
void lc_ucol_close(UCollator* coll) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCollator*);
  ptr = (void(*)(UCollator*))syms[3];
  ptr(coll);
  #else
  ucol_close(coll);
  #endif
}

LOCAL_INLINE
UCollationResult lc_ucol_strcoll(const UCollator* coll, const UChar* source, int32_t sourceLength, const UChar* target, int32_t targetLength) {
    #ifdef CBL_USE_ICU_SHIM
    pthread_once(&once_control, &init_icudata_version);
    UCollationResult (*ptr)(const UCollator*, const UChar*, int32_t, const UChar*, int32_t);
    ptr = (UCollationResult(*)(const UCollator*, const UChar*, int32_t, const UChar*, int32_t))syms[4];
    return ptr(coll, source, sourceLength, target, targetLength);
    #else
    return ucol_strcoll(coll, source, sourceLength, target, targetLength);
    #endif
}

LOCAL_INLINE
UCollationResult lc_ucol_strcollIter(const UCollator* coll, UCharIterator* sIter, UCharIterator* tIter, UErrorCode* status) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  UCollationResult (*ptr)(const UCollator*, UCharIterator*, UCharIterator*, UErrorCode*);
  if (syms[10] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return (UCollationResult)0;
  }
  ptr = (UCollationResult(*)(const UCollator*, UCharIterator*, UCharIterator*, UErrorCode*))syms[10];
  return ptr(coll, sIter, tIter, status);
  #else
  return ucol_strcollIter(coll, sIter, tIter, status);
  #endif
}

/* unicode/uiter.h */
LOCAL_INLINE
void lc_uiter_setUTF8(UCharIterator* iter, const char* s, int32_t length) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCharIterator*, const char*, int32_t);
  ptr = (void(*)(UCharIterator*, const char*, int32_t))syms[9];
  ptr(iter, s, length);
  #else
  uiter_setUTF8(iter, s, length);
  #endif
}

/* unicode/ucasemap.h */
LOCAL_INLINE
UCaseMap* lc_ucasemap_open(const char* locale, uint32_t options, UErrorCode* pErrorCode) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  UCaseMap* (*ptr)(const char*, uint32_t, UErrorCode*);
  if (syms[5] == NULL) {
    *pErrorCode = U_UNSUPPORTED_ERROR;
    return (UCaseMap*)0;
  }
  ptr = (UCaseMap*(*)(const char*, uint32_t, UErrorCode*))syms[5];
  return ptr(locale, options, pErrorCode);
  #else
  return ucasemap_open(locale, options, pErrorCode);
  #endif
}

LOCAL_INLINE
void lc_ucasemap_close(UCaseMap* csm) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCaseMap*);
  ptr = (void(*)(UCaseMap*))syms[6];
  ptr(csm);
  #else
  ucasemap_close(csm);
  #endif
}

LOCAL_INLINE
int32_t lc_ucasemap_utf8ToLower(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  int32_t (*ptr)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*);
  if (syms[7] == NULL) {
    *pErrorCode = U_UNSUPPORTED_ERROR;
    return (int32_t)0;
  }
  ptr = (int32_t(*)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*))syms[7];
  return ptr(csm, dest, destCapacity, src, srcLength, pErrorCode);
  #else
  return ucasemap_utf8ToLower(csm, dest, destCapacity, src, srcLength, pErrorCode);
  #endif
}

LOCAL_INLINE
int32_t lc_ucasemap_utf8ToUpper(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode) {
  #ifdef CBL_USE_ICU_SHIM
  pthread_once(&once_control, &init_icudata_version);
  int32_t (*ptr)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*);
  if (syms[8] == NULL) {
    *pErrorCode = U_UNSUPPORTED_ERROR;
    return (int32_t)0;
  }
  ptr = (int32_t(*)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*))syms[8];
  return ptr(csm, dest, destCapacity, src, srcLength, pErrorCode);
  #else
  return ucasemap_utf8ToUpper(csm, dest, destCapacity, src, srcLength, pErrorCode);
  #endif
}

#undef LOCAL_INLINE