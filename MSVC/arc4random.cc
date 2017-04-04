#include <random>
#include <cstdint>

std::random_device rd;
std::default_random_engine e(rd());

extern "C" {
	uint32_t arc4random()
	{
		return e();
	}
}