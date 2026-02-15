#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include "ui/screen_manager.h"
#include <functional>
#include <string>
#include <vector>

namespace UI {

enum class SettingItemType { INTEGER, TOGGLE, ACTION };

struct SettingItem {
  std::string label;
  std::string description;
  SettingItemType type;
  int value;
  int min;
  int max;
  std::function<std::string(int)> valueFormatter;
  std::function<void(int)> onUpdate;
};

class SettingsScreen : public Screen {
public:
  SettingsScreen();
  virtual ~SettingsScreen();

  void onEnter() override;
  void onExit() override;
  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;

private:
  std::vector<SettingItem> items;
  int selectedIndex;
  float scrollOffset;

  void saveAndExit();
};

} // namespace UI

#endif // SETTINGS_SCREEN_H
