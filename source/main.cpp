#include "core/config.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "log.h"
#include "network/http_client.h"
#include "network/network_manager.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include "utils/sound_player.h"
#include "utils/system_utils.h"
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <malloc.h>

static const size_t SOC_SHAREDMEM_SIZE = 0x200000;
static u32 *soc_sharedmem_ptr = NULL;

extern "C" {
extern char *fake_heap_start;
extern char *fake_heap_end;
extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;

// libctru's version, but a 64MB process keeps 24MB linear so the heap gets the rest.
void __system_allocateHeaps(void) {
	const u32 MB = 1024 * 1024;

	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
		svcBreak(USERBREAK_PANIC);
	}
	ResourceLimitType commit = RESLIMIT_COMMIT;
	s64 limit = 0;
	s64 used = 0;
	svcGetResourceLimitLimitValues(&limit, reslimit, &commit, 1);
	svcGetResourceLimitCurrentValues(&used, reslimit, &commit, 1);
	svcCloseHandle(reslimit);

	u32 remaining = (u32)(limit - used) & ~0xFFF;
	u32 linearCap = remaining > 64 * MB ? 32 * MB : 24 * MB;
	u32 linear = (remaining / 2) & ~0xFFF;
	if (linear > linearCap) {
		linear = linearCap;
	}
	__ctru_linear_heap_size = linear;
	__ctru_heap_size = remaining - linear;

	if (R_FAILED(
	        svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READWRITE))) {
		svcBreak(USERBREAK_PANIC);
	}
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
	                              MEMPERM_READWRITE))) {
		svcBreak(USERBREAK_PANIC);
	}

	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

	fake_heap_start = (char *)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}

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
	Logger::log("[Mem] app region %luKB (free %luKB), heap %luKB, linear %luKB (free %luKB)",
	            (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
	            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
	            (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(__ctru_linear_heap_size / 1024),
	            (unsigned long)(linearSpaceFree() / 1024));
	Config::getInstance().load();
	if (Utils::System::isNew3DS()) {
		Network::NetworkManager::getInstance().init(3, 2);
	} else {
		Network::NetworkManager::getInstance().init(2, 1);
	}

	Network::HttpClient timeClient;
	timeClient.setTimeout(3);
	Network::HttpResponse resp = timeClient.get("http://detectportal.firefox.com/success.txt", {});
	if (resp.headers.count("Date")) {
		UI::MessageUtils::syncClock(resp.headers["Date"]);
	}

	UI::ImageManager::getInstance().init();
	Discord::DiscordClient::getInstance().init();
	UI::ScreenManager::getInstance().init();

	unsigned callFrame = 0;
	while (aptMainLoop()) {
		hidScanInput();

		UI::ScreenManager::getInstance().update();
		Discord::DiscordClient::getInstance().update();

		if (UI::ScreenManager::getInstance().shouldCloseApplication()) {
			break;
		}

		// During a call the CPU is better spent on audio: draw every other frame
		// (30 fps) and sleep through the other vblank. Input and logic still run
		// at full rate.
		const bool inCall = Discord::VoiceClient::getInstance().getState() == Discord::VoiceState::ESTABLISHED;
		if ((inCall && (++callFrame & 1)) || !UI::ScreenManager::getInstance().shouldRender()) {
			gspWaitForVBlank();
		} else {
			UI::ScreenManager::getInstance().render();
		}
	}

	// Its destructor would otherwise run after socExit and ndspExit.
	Discord::VoiceClient::getInstance().disconnect();

	// Network first: its workers run callbacks into everything below.
	Network::NetworkManager::getInstance().shutdown();
	Discord::DiscordClient::getInstance().shutdown();
	UI::ScreenManager::getInstance().shutdown();
	UI::ImageManager::getInstance().shutdown();
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
