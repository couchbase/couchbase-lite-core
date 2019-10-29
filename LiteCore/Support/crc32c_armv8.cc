//
// crc32c_armv8.cc
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

#ifdef __ARM_FEATURE_CRC32

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <arm_acle.h>
#include "crc32c.h"

uint32_t crc32c_hw(uint32_t crc, const uint8_t *p, unsigned int len)
{
	int64_t length = len;

	while ((length -= sizeof(uint64_t)) >= 0) {
		__crc32x(crc, *((uint64_t *)p));
		p += sizeof(uint64_t);
	}

	if (length & sizeof(uint32_t)) {
		__crc32cw(crc, *((uint32_t *)p));
		p += sizeof(uint32_t);
	}

	if (length & sizeof(uint16_t)) {
		__crc32ch(crc, *((uint16_t *)p));
		p += sizeof(uint16_t);
	}
    
	if (length & sizeof(uint8_t))
		__crc32b(crc, *p);

	return crc;
}

#endif
