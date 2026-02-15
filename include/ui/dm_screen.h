#ifndef DM_SCREEN_H
#define DM_SCREEN_H

#include "discord/types.h"
#include "ui/screen_manager.h"
#include <string>
#include <vector>

namespace UI {

class DmScreen : public Screen {
public:
  DmScreen();
  virtual ~DmScreen() = default;

  virtual void update() override;
  virtual void renderTop(C3D_RenderTarget *target) override;
  virtual void renderBottom(C3D_RenderTarget *target) override;

private:
  void refreshDms();
  void drawDmItem(int index, const Discord::Channel &dm, float y);

  std::vector<Discord::Channel> dms;
  int selectedIndex;
  int scrollOffset;
  int repeatTimer;
  int lastKey;

  static const float ITEM_HEIGHT;
  static const int VISIBLE_ITEMS;
  static const int REPEAT_DELAY_INITIAL = 30;
  static const int REPEAT_DELAY_CONTINUOUS = 6;
};

} // namespace UI

#endif // DM_SCREEN_H
