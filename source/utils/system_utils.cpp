#include "utils/system_utils.h"
#include "log.h"
#include <3ds.h>
#include <malloc.h>

extern "C" u32 __ctru_heap_size;
extern "C" u32 __ctru_linear_heap_size;

namespace Utils {
namespace System {

static std::string detectDeviceModelName() {
	std::string name = "Nintendo 3DS";
	if (R_SUCCEEDED(cfguInit())) {
		u8 model = 0;
		if (R_SUCCEEDED(CFGU_GetSystemModel(&model))) {
			switch (model) {
			case CFG_MODEL_3DS:
				name = "Nintendo 3DS";
				break;
			case CFG_MODEL_3DSXL:
				name = "Nintendo 3DS XL";
				break;
			case CFG_MODEL_N3DS:
				name = "New Nintendo 3DS";
				break;
			case CFG_MODEL_2DS:
				name = "Nintendo 2DS";
				break;
			case CFG_MODEL_N3DSXL:
				name = "New Nintendo 3DS XL";
				break;
			case CFG_MODEL_N2DSXL:
				name = "New Nintendo 2DS XL";
				break;
			}
		}
		cfguExit();
	}
	return name;
}

const std::string &getDeviceModelName() {
	static const std::string name = detectDeviceModelName();
	return name;
}

bool isNew3DS() {
	static const bool isNew = [] {
		bool value = false;
		Result rc = APT_CheckNew3DS(&value);
		Logger::log("[System] APT_CheckNew3DS rc=0x%08lX new3ds=%d", (unsigned long)rc, value ? 1 : 0);
		return R_SUCCEEDED(rc) && value;
	}();
	return isNew;
}

// Heap that newlib's arena has not claimed yet is free too.
size_t heapFreeBytes() {
	struct mallinfo info = mallinfo();
	size_t arena = (size_t)info.arena;
	size_t untouched = __ctru_heap_size > arena ? __ctru_heap_size - arena : 0;
	return untouched + (size_t)info.fordblks;
}

size_t linearHeapSize() { return __ctru_linear_heap_size; }

} // namespace System
} // namespace Utils
