#include "utils/system_utils.h"
#include <3ds.h>

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

} // namespace System
} // namespace Utils
