#ifndef DISCLAIMER_SCREEN_H
#define DISCLAIMER_SCREEN_H

#include "screen_manager.h"
#include <string>

namespace UI {

class DisclaimerScreen : public Screen {
public:
  DisclaimerScreen();
  ~DisclaimerScreen() override = default;

  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;
  void onEnter() override;

private:
  static const std::string DISCLAIMER_TEXT;
};

} // namespace UI

#endif // DISCLAIMER_SCREEN_H
