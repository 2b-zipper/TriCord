#ifndef HAMBURGER_MENU_H
#define HAMBURGER_MENU_H

#include <citro2d.h>
#include <string>
#include <vector>

namespace UI {

enum class MenuItemType {
  SERVER_LIST,
  SETTINGS,
  HOME,
  DIRECT_MESSAGES,
  ACCOUNT_SWITCH,
  ABOUT
};

struct MenuItem {
  std::string label;
  MenuItemType type;
};

class HamburgerMenu {
public:
  HamburgerMenu();
  ~HamburgerMenu() = default;

  void update();
  void render();

  void toggle();
  void open();
  void close();
  void reset();
  bool isOpen() const {
    return state == State::OPEN || state == State::OPENING;
  }
  bool isClosed() const { return state == State::CLOSED; }
  void refreshStrings();

private:
  enum class State {
    CLOSED,
    OPENING,
    OPEN,
    CLOSING,
    ACCOUNT_SELECTION,
    STATUS_SELECTION,
    DELETE_CONFIRMATION
  };

  State state;
  float slideProgress;
  int selectedIndex;

  int accountSelectionIndex;
  int accountScrollOffset;
  int statusSelectionIndex = 0;

  std::vector<MenuItem> items;

  static const float MENU_WIDTH;
  static const float ANIMATION_SPEED;

  void drawMenuItem(int index, float y, float alpha);
  void drawAccountCard(float x, float y, float alpha);
  bool accountCardSelected = false;
};

} // namespace UI

#endif // HAMBURGER_MENU_H
