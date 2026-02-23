#include "core/config.h"
#include "core/i18n.h"
#include "discord/discord_client.h"
#include "log.h"
#include "network/network_manager.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
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

  Logger::init();
  Logger::log("TriCord - Discord for 3DS starting...");
  Config::getInstance().load();
  Network::NetworkManager::getInstance().init(3, 2);
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
  Network::NetworkManager::getInstance().shutdown();
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
