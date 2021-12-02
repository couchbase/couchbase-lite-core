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
 */

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#include "unicode/utypes.h"
#include "unicode/ucol.h"
#include "unicode/ucasemap.h"

/* Allowed version number ranges between [44, 999].
 * 44 is the minimum supported ICU version that was shipped in
 * Gingerbread (2.3.3) devices.
 */
#define ICUDATA_VERSION_MIN_LENGTH 2
#define ICUDATA_VERSION_MAX_LENGTH 3
#define ICUDATA_VERSION_MIN        44

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static char icudata_version[ICUDATA_VERSION_MAX_LENGTH + 1];

static void* handle_i18n = NULL;
static void* handle_common = NULL;
static void* syms[11];

/* ICU data filename on Android is like 'icudt49l.dat'.
 *
 * The following is from ICU code: source/common/unicode/utypes.h:
 * #define U_ICUDATA_NAME "icudt" U_ICU_VERSION_SHORT U_ICUDATA_TYPE_LETTER
 *
 * U_ICUDATA_TYPE_LETTER needs to be 'l' as it's always little-endian on
 * Android devices.
 *
 * U_ICU_VERSION_SHORT is a decimal number between [44, 999].
 */
static int filter(const struct dirent* dirp) {
  const char* name = dirp->d_name;
  const int len = strlen(name);

  // Valid length of the filename 'icudt...l.dat'
  if (len < 10 + ICUDATA_VERSION_MIN_LENGTH ||
      len > 10 + ICUDATA_VERSION_MAX_LENGTH) {
    return 0;
  }

  // Valid decimal number in between
  for (int i = 5; i < len - 5; i++) {
    if (!isdigit(name[i])) {
      return 0;
    }
  }

  return !strncmp(name, "icudt", 5) && !strncmp(&name[len - 5], "l.dat", 5);
}

static void init_icudata_version() {
  memset(icudata_version, 0, ICUDATA_VERSION_MAX_LENGTH + 1);
  memset(syms, 0, sizeof(syms));

  struct dirent** namelist = NULL;
  int n = scandir("/system/usr/icu", &namelist, &filter, alphasort);
  int max_version = -1;
  while (n--) {
    char* name = namelist[n]->d_name;
    const int len = strlen(name);
    const char* verp = &name[5];
    name[len - 5] = '\0';

    char* endptr;
    int ver = (int)strtol(verp, &endptr, 10);

    // We prefer the latest version available.
    if (ver > max_version) {
      max_version = ver;
      icudata_version[0] = '_';
      strcpy(icudata_version + 1, verp);
    }

    free(namelist[n]);
  }
  free(namelist);

  if (max_version == -1 || max_version < ICUDATA_VERSION_MIN) {
    __android_log_print(ANDROID_LOG_ERROR, "NDKICU",
        "Cannot locate ICU data file at /system/usr/icu.");
    return;
  }

  handle_i18n = dlopen("libicui18n.so", RTLD_LOCAL);
  handle_common = dlopen("libicuuc.so", RTLD_LOCAL);

  if (!handle_i18n || !handle_common) {
    __android_log_print(ANDROID_LOG_ERROR, "NDKICU", "Cannot open ICU libraries.");
    return;
  }

    char func_name[128];
    strcpy(func_name, "ucol_open");
    strcat(func_name, icudata_version);
    syms[0] = dlsym(handle_i18n, func_name);

    strcpy(func_name, "ucol_setAttribute");
    strcat(func_name, icudata_version);
    syms[1] = dlsym(handle_i18n, func_name);

    strcpy(func_name, "ucol_strcollUTF8");
    strcat(func_name, icudata_version);
    syms[2] = dlsym(handle_i18n, func_name);

    strcpy(func_name, "ucol_close");
    strcat(func_name, icudata_version);
    syms[3] = dlsym(handle_i18n, func_name);

    strcpy(func_name, "ucol_strcoll");
    strcat(func_name, icudata_version);
    syms[4] = dlsym(handle_i18n, func_name);

    strcpy(func_name, "ucasemap_open");
    strcat(func_name, icudata_version);
    syms[5] = dlsym(handle_common, func_name);

    strcpy(func_name, "ucasemap_close");
    strcat(func_name, icudata_version);
    syms[6] = dlsym(handle_common, func_name);

    strcpy(func_name, "ucasemap_utf8ToLower");
    strcat(func_name, icudata_version);
    syms[7] = dlsym(handle_common, func_name);

    strcpy(func_name, "ucasemap_utf8ToUpper");
    strcat(func_name, icudata_version);
    syms[8] = dlsym(handle_common, func_name);

    strcpy(func_name, "uiter_setUTF8");
    strcat(func_name, icudata_version);
    syms[9] = dlsym(handle_common, func_name);

    strcpy(func_name, "ucol_strcollIter");
    strcat(func_name, icudata_version);
    syms[10] = dlsym(handle_i18n, func_name);
}

UCollator* lc_ucol_open(const char* loc, UErrorCode* status) {
  pthread_once(&once_control, &init_icudata_version);
  UCollator* (*ptr)(const char*, UErrorCode*);
  if (syms[0] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return (UCollator*)0;
  }
  ptr = (UCollator*(*)(const char*, UErrorCode*))syms[0];
  return ptr(loc, status);
}

void lc_ucol_setAttribute(UCollator* coll, UColAttribute attr, UColAttributeValue value, UErrorCode* status) {
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCollator*, UColAttribute, UColAttributeValue, UErrorCode*);
  if (syms[1] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return;
  }
  ptr = (void(*)(UCollator*, UColAttribute, UColAttributeValue, UErrorCode*))syms[1];
  ptr(coll, attr, value, status);
  return;
}

UCollationResult lc_ucol_strcollUTF8(const UCollator* coll, const char* source, int32_t sourceLength, const char* target, int32_t targetLength, UErrorCode* status) {
  pthread_once(&once_control, &init_icudata_version);
  UCollationResult (*ptr)(const UCollator*, const char*, int32_t, const char*, int32_t, UErrorCode*);
  if (syms[2] == NULL) {
    *status = U_UNSUPPORTED_ERROR;
    return (UCollationResult)0;
  }
  ptr = (UCollationResult(*)(const UCollator*, const char*, int32_t, const char*, int32_t, UErrorCode*))syms[2];
  return ptr(coll, source, sourceLength, target, targetLength, status);
}

void lc_ucol_close(UCollator* coll) {
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCollator*);
  ptr = (void(*)(UCollator*))syms[3];
  ptr(coll);
}

UCollationResult lc_ucol_strcoll(const UCollator* coll, const UChar* source, int32_t sourceLength, const UChar* target, int32_t targetLength) {
    pthread_once(&once_control, &init_icudata_version);
    UCollationResult (*ptr)(const UCollator*, const UChar*, int32_t, const UChar*, int32_t);
    ptr = (UCollationResult(*)(const UCollator*, const UChar*, int32_t, const UChar*, int32_t))syms[4];
    return ptr(coll, source, sourceLength, target, targetLength);
}

/* unicode/ucasemap.h */
UCaseMap* lc_ucasemap_open(const char* locale, uint32_t options, UErrorCode* pErrorCode) {
  pthread_once(&once_control, &init_icudata_version);
  UCaseMap* (*ptr)(const char*, uint32_t, UErrorCode*);
  if (syms[5] == NULL) {
    *pErrorCode = U_UNSUPPORTED_ERROR;
    return (UCaseMap*)0;
  }
  ptr = (UCaseMap*(*)(const char*, uint32_t, UErrorCode*))syms[5];
  return ptr(locale, options, pErrorCode);
}

void lc_ucasemap_close(UCaseMap* csm) {
  pthread_once(&once_control, &init_icudata_version);
  void (*ptr)(UCaseMap*);
  ptr = (void(*)(UCaseMap*))syms[6];
  ptr(csm);
  return;
}

int32_t lc_ucasemap_utf8ToLower(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode) {
  pthread_once(&once_control, &init_icudata_version);
  int32_t (*ptr)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*);
  if (syms[7] == NULL) {
    *pErrorCode = U_UNSUPPORTED_ERROR;
    return (int32_t)0;
  }
  ptr = (int32_t(*)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*))syms[7];
  return ptr(csm, dest, destCapacity, src, srcLength, pErrorCode);
}

int32_t lc_ucasemap_utf8ToUpper(const UCaseMap* csm, char* dest, int32_t destCapacity, const char* src, int32_t srcLength, UErrorCode* pErrorCode) {
  pthread_once(&once_control, &init_icudata_version);
  int32_t (*ptr)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*);
  if (syms[8] == NULL) {
    *pErrorCode = U_UNSUPPORTED_ERROR;
    return (int32_t)0;
  }
  ptr = (int32_t(*)(const UCaseMap*, char*, int32_t, const char*, int32_t, UErrorCode*))syms[8];
  return ptr(csm, dest, destCapacity, src, srcLength, pErrorCode);
}
