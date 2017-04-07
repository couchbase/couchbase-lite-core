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