#include "core/config.h"
#include "discord/discord_client.h"
#include "log.h"
#include "network/http_client.h"
#include "network/network_manager.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include "utils/sound_player.h"
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <malloc.h>

static const size_t SOC_SHAREDMEM_SIZE = 0x200000;
static u32 *soc_sharedmem_ptr = NULL;

int main(int argc, char **argv) {
	osSetSpeedupEnable(true);
	gfxInitDefault();

	soc_sharedmem_ptr = (u32 *)memalign(0x1000, SOC_SHAREDMEM_SIZE);
	if (soc_sharedmem_ptr) {
		socInit(soc_sharedmem_ptr, SOC_SHAREDMEM_SIZE);
	}

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

	romfsInit();
	psInit();

	if (R_SUCCEEDED(ndspInit())) {
		ndspSetOutputMode(NDSP_OUTPUT_MONO);
		Utils::SoundPlayer::getInstance().init();
	} else {
		Logger::log("Audio unavailable (is dspfirm.cdc dumped?)");
	}

	Logger::init();
	Logger::log("TriCord - Discord for 3DS starting...");
	Logger::log("[Mem] app region %luKB (free %luKB), linear free %luKB",
	            (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
	            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
	            (unsigned long)(linearSpaceFree() / 1024));
	Config::getInstance().load();
	Network::NetworkManager::getInstance().init(3, 2);

	Network::HttpClient timeClient;
	timeClient.setTimeout(3);
	Network::HttpResponse resp = timeClient.get("http://detectportal.firefox.com/success.txt", {});
	if (resp.headers.count("Date")) {
		UI::MessageUtils::syncClock(resp.headers["Date"]);
	}

	UI::ImageManager::getInstance().init();
	Discord::DiscordClient::getInstance().init();
	UI::ScreenManager::getInstance().init();

	while (aptMainLoop()) {
		hidScanInput();

		UI::ScreenManager::getInstance().update();
		Discord::DiscordClient::getInstance().update();

		if (UI::ScreenManager::getInstance().shouldCloseApplication()) {
			break;
		}

		UI::ScreenManager::getInstance().render();
	}

	UI::ScreenManager::getInstance().shutdown();
	UI::ImageManager::getInstance().shutdown();
	Discord::DiscordClient::getInstance().shutdown();
	Network::NetworkManager::getInstance().shutdown();
	Utils::SoundPlayer::getInstance().shutdown();
	ndspExit();
	psExit();
	romfsExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();

	if (soc_sharedmem_ptr) {
		socExit();
		free(soc_sharedmem_ptr);
	}

	return 0;
}
