//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include <random>
#include <cstdint>

std::random_device rd;
std::default_random_engine e(rd());



extern "C" {
	typedef union
	{
		int i;
		unsigned char b[4];
	} int_sandwich;

	uint32_t arc4random()
	{
		return e();
	}

	uint32_t arc4random_uniform(uint32_t upperBound)
	{
		std::uniform_int_distribution<uint32_t> uniform(0, upperBound - 1);
		return uniform(e);
	}

	void arc4random_buf(void *buffer, int size)
	{
		unsigned char* buf = (unsigned char *)buffer;
		int_sandwich sand;
		int offset = 0;
		while (offset < size) {
			sand.i = e();
			for (int i = 0; i < 4; i++) {
				if (offset >= size) {
					break;
				}

				*buf = sand.b[i];
				buf++;
				offset++;
			}
		}
	}
}