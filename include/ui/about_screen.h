#ifndef ABOUT_SCREEN_H
#define ABOUT_SCREEN_H

#include "screen_manager.h"
#include <string>

namespace UI {

class AboutScreen : public Screen {
public:
  AboutScreen();
  ~AboutScreen() override = default;

  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;
  void onEnter() override;

private:
  float animTimer;
  float logoBounce;
  float scrollOffset;
  float maxScroll;
};

} // namespace UI

#endif // ABOUT_SCREEN_H
