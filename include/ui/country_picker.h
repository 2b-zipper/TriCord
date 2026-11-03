#ifndef COUNTRY_PICKER_H
#define COUNTRY_PICKER_H

namespace UI {

struct Country {
	const char *iso;
	const char *name;
	const char *dialCode;
};

class CountryPicker {
  public:
	CountryPicker();

	const Country &selected() const;
	bool isOpen() const { return open; }
	void show();
	void update();
	void render();

  private:
	int selectedIndex = 0;
	int cursor = 0;
	float scroll = 0.0f;
	bool open = false;

	int repeatTimer = 0;
	bool dragging = false;
	bool draggingScrollbar = false;
	bool tapCandidate = false;
	float touchStartY = 0.0f;
	float lastTouchY = 0.0f;

	void select(int index);
	void moveCursor(int delta);
	void scrollTo(float target);
};

} // namespace UI

#endif // COUNTRY_PICKER_H
