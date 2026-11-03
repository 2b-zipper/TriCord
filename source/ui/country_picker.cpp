#include "ui/country_picker.h"
#include "config.h"
#include "core/i18n.h"
#include "ui/screen_manager.h"
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace UI {

namespace {
const Country COUNTRIES[] = {
    {"AF", "Afghanistan", "93"},
    {"AL", "Albania", "355"},
    {"DZ", "Algeria", "213"},
    {"AS", "American Samoa", "1"},
    {"AD", "Andorra", "376"},
    {"AO", "Angola", "244"},
    {"AI", "Anguilla", "1"},
    {"AG", "Antigua and Barbuda", "1"},
    {"AR", "Argentina", "54"},
    {"AM", "Armenia", "374"},
    {"AW", "Aruba", "297"},
    {"AU", "Australia", "61"},
    {"AT", "Austria", "43"},
    {"AZ", "Azerbaijan", "994"},
    {"BS", "Bahamas", "1"},
    {"BH", "Bahrain", "973"},
    {"BD", "Bangladesh", "880"},
    {"BB", "Barbados", "1"},
    {"BY", "Belarus", "375"},
    {"BE", "Belgium", "32"},
    {"BZ", "Belize", "501"},
    {"BJ", "Benin", "229"},
    {"BM", "Bermuda", "1"},
    {"BT", "Bhutan", "975"},
    {"BO", "Bolivia", "591"},
    {"BA", "Bosnia and Herzegovina", "387"},
    {"BW", "Botswana", "267"},
    {"BR", "Brazil", "55"},
    {"IO", "British Indian Ocean Territory", "246"},
    {"VG", "British Virgin Islands", "1"},
    {"BN", "Brunei", "673"},
    {"BG", "Bulgaria", "359"},
    {"BF", "Burkina Faso", "226"},
    {"BI", "Burundi", "257"},
    {"KH", "Cambodia", "855"},
    {"CM", "Cameroon", "237"},
    {"CA", "Canada", "1"},
    {"CV", "Cape Verde", "238"},
    {"KY", "Cayman Islands", "1"},
    {"CF", "Central African Republic", "236"},
    {"TD", "Chad", "235"},
    {"CL", "Chile", "56"},
    {"CN", "China", "86"},
    {"CO", "Colombia", "57"},
    {"KM", "Comoros", "269"},
    {"CG", "Congo - Brazzaville", "242"},
    {"CD", "Congo - Kinshasa", "243"},
    {"CK", "Cook Islands", "682"},
    {"CR", "Costa Rica", "506"},
    {"CI", "Cote d'Ivoire", "225"},
    {"HR", "Croatia", "385"},
    {"CU", "Cuba", "53"},
    {"CW", "Curacao", "599"},
    {"CY", "Cyprus", "357"},
    {"CZ", "Czechia", "420"},
    {"DK", "Denmark", "45"},
    {"DJ", "Djibouti", "253"},
    {"DM", "Dominica", "1"},
    {"DO", "Dominican Republic", "1"},
    {"EC", "Ecuador", "593"},
    {"EG", "Egypt", "20"},
    {"SV", "El Salvador", "503"},
    {"GQ", "Equatorial Guinea", "240"},
    {"ER", "Eritrea", "291"},
    {"EE", "Estonia", "372"},
    {"SZ", "Eswatini", "268"},
    {"ET", "Ethiopia", "251"},
    {"FK", "Falkland Islands", "500"},
    {"FO", "Faroe Islands", "298"},
    {"FJ", "Fiji", "679"},
    {"FI", "Finland", "358"},
    {"FR", "France", "33"},
    {"GF", "French Guiana", "594"},
    {"PF", "French Polynesia", "689"},
    {"GA", "Gabon", "241"},
    {"GM", "Gambia", "220"},
    {"GE", "Georgia", "995"},
    {"DE", "Germany", "49"},
    {"GH", "Ghana", "233"},
    {"GI", "Gibraltar", "350"},
    {"GR", "Greece", "30"},
    {"GL", "Greenland", "299"},
    {"GD", "Grenada", "1"},
    {"GP", "Guadeloupe", "590"},
    {"GU", "Guam", "1"},
    {"GT", "Guatemala", "502"},
    {"GG", "Guernsey", "44"},
    {"GN", "Guinea", "224"},
    {"GW", "Guinea-Bissau", "245"},
    {"GY", "Guyana", "592"},
    {"HT", "Haiti", "509"},
    {"HN", "Honduras", "504"},
    {"HK", "Hong Kong", "852"},
    {"HU", "Hungary", "36"},
    {"IS", "Iceland", "354"},
    {"IN", "India", "91"},
    {"ID", "Indonesia", "62"},
    {"IR", "Iran", "98"},
    {"IQ", "Iraq", "964"},
    {"IE", "Ireland", "353"},
    {"IM", "Isle of Man", "44"},
    {"IL", "Israel", "972"},
    {"IT", "Italy", "39"},
    {"JM", "Jamaica", "1"},
    {"JP", "Japan", "81"},
    {"JE", "Jersey", "44"},
    {"JO", "Jordan", "962"},
    {"KZ", "Kazakhstan", "7"},
    {"KE", "Kenya", "254"},
    {"KI", "Kiribati", "686"},
    {"XK", "Kosovo", "383"},
    {"KW", "Kuwait", "965"},
    {"KG", "Kyrgyzstan", "996"},
    {"LA", "Laos", "856"},
    {"LV", "Latvia", "371"},
    {"LB", "Lebanon", "961"},
    {"LS", "Lesotho", "266"},
    {"LR", "Liberia", "231"},
    {"LY", "Libya", "218"},
    {"LI", "Liechtenstein", "423"},
    {"LT", "Lithuania", "370"},
    {"LU", "Luxembourg", "352"},
    {"MO", "Macao", "853"},
    {"MG", "Madagascar", "261"},
    {"MW", "Malawi", "265"},
    {"MY", "Malaysia", "60"},
    {"MV", "Maldives", "960"},
    {"ML", "Mali", "223"},
    {"MT", "Malta", "356"},
    {"MH", "Marshall Islands", "692"},
    {"MQ", "Martinique", "596"},
    {"MR", "Mauritania", "222"},
    {"MU", "Mauritius", "230"},
    {"YT", "Mayotte", "262"},
    {"MX", "Mexico", "52"},
    {"FM", "Micronesia", "691"},
    {"MD", "Moldova", "373"},
    {"MC", "Monaco", "377"},
    {"MN", "Mongolia", "976"},
    {"ME", "Montenegro", "382"},
    {"MS", "Montserrat", "1"},
    {"MA", "Morocco", "212"},
    {"MZ", "Mozambique", "258"},
    {"MM", "Myanmar", "95"},
    {"NA", "Namibia", "264"},
    {"NR", "Nauru", "674"},
    {"NP", "Nepal", "977"},
    {"NL", "Netherlands", "31"},
    {"NC", "New Caledonia", "687"},
    {"NZ", "New Zealand", "64"},
    {"NI", "Nicaragua", "505"},
    {"NE", "Niger", "227"},
    {"NG", "Nigeria", "234"},
    {"NU", "Niue", "683"},
    {"KP", "North Korea", "850"},
    {"MK", "North Macedonia", "389"},
    {"MP", "Northern Mariana Islands", "1"},
    {"NO", "Norway", "47"},
    {"OM", "Oman", "968"},
    {"PK", "Pakistan", "92"},
    {"PW", "Palau", "680"},
    {"PS", "Palestine", "970"},
    {"PA", "Panama", "507"},
    {"PG", "Papua New Guinea", "675"},
    {"PY", "Paraguay", "595"},
    {"PE", "Peru", "51"},
    {"PH", "Philippines", "63"},
    {"PL", "Poland", "48"},
    {"PT", "Portugal", "351"},
    {"PR", "Puerto Rico", "1"},
    {"QA", "Qatar", "974"},
    {"RE", "Reunion", "262"},
    {"RO", "Romania", "40"},
    {"RU", "Russia", "7"},
    {"RW", "Rwanda", "250"},
    {"SH", "Saint Helena", "290"},
    {"KN", "Saint Kitts and Nevis", "1"},
    {"LC", "Saint Lucia", "1"},
    {"PM", "Saint Pierre and Miquelon", "508"},
    {"VC", "Saint Vincent and the Grenadines", "1"},
    {"WS", "Samoa", "685"},
    {"SM", "San Marino", "378"},
    {"ST", "Sao Tome and Principe", "239"},
    {"SA", "Saudi Arabia", "966"},
    {"SN", "Senegal", "221"},
    {"RS", "Serbia", "381"},
    {"SC", "Seychelles", "248"},
    {"SL", "Sierra Leone", "232"},
    {"SG", "Singapore", "65"},
    {"SX", "Sint Maarten", "1"},
    {"SK", "Slovakia", "421"},
    {"SI", "Slovenia", "386"},
    {"SB", "Solomon Islands", "677"},
    {"SO", "Somalia", "252"},
    {"ZA", "South Africa", "27"},
    {"KR", "South Korea", "82"},
    {"SS", "South Sudan", "211"},
    {"ES", "Spain", "34"},
    {"LK", "Sri Lanka", "94"},
    {"SD", "Sudan", "249"},
    {"SR", "Suriname", "597"},
    {"SE", "Sweden", "46"},
    {"CH", "Switzerland", "41"},
    {"SY", "Syria", "963"},
    {"TW", "Taiwan", "886"},
    {"TJ", "Tajikistan", "992"},
    {"TZ", "Tanzania", "255"},
    {"TH", "Thailand", "66"},
    {"TL", "Timor-Leste", "670"},
    {"TG", "Togo", "228"},
    {"TK", "Tokelau", "690"},
    {"TO", "Tonga", "676"},
    {"TT", "Trinidad and Tobago", "1"},
    {"TN", "Tunisia", "216"},
    {"TR", "Turkey", "90"},
    {"TM", "Turkmenistan", "993"},
    {"TC", "Turks and Caicos Islands", "1"},
    {"TV", "Tuvalu", "688"},
    {"UG", "Uganda", "256"},
    {"UA", "Ukraine", "380"},
    {"AE", "United Arab Emirates", "971"},
    {"GB", "United Kingdom", "44"},
    {"US", "United States", "1"},
    {"VI", "U.S. Virgin Islands", "1"},
    {"UY", "Uruguay", "598"},
    {"UZ", "Uzbekistan", "998"},
    {"VU", "Vanuatu", "678"},
    {"VA", "Vatican City", "39"},
    {"VE", "Venezuela", "58"},
    {"VN", "Vietnam", "84"},
    {"WF", "Wallis and Futuna", "681"},
    {"YE", "Yemen", "967"},
    {"ZM", "Zambia", "260"},
    {"ZW", "Zimbabwe", "263"},
};
constexpr int COUNTRY_COUNT = (int)std::size(COUNTRIES);

constexpr float ROW_H = 24.0f;
constexpr int VISIBLE_ROWS = 6;
constexpr float HEADER_H = 34.0f;
constexpr float FOOTER_H = 26.0f;
constexpr float LIST_H = ROW_H * VISIBLE_ROWS;
constexpr float POPUP_W = 280.0f;
constexpr float POPUP_H = HEADER_H + LIST_H + FOOTER_H;
constexpr float POPUP_X = (BOTTOM_SCREEN_WIDTH - POPUP_W) / 2.0f;
constexpr float POPUP_Y = (BOTTOM_SCREEN_HEIGHT - POPUP_H) / 2.0f;
constexpr float POPUP_RADIUS = 12.0f;
constexpr float LIST_TOP = POPUP_Y + HEADER_H;
constexpr float LIST_BOTTOM = LIST_TOP + LIST_H;
constexpr float MAX_SCROLL = COUNTRY_COUNT * ROW_H - LIST_H;
constexpr float SCROLLBAR_TOUCH_W = 24.0f;
constexpr float TAP_SLOP = 8.0f;

constexpr int REPEAT_INITIAL_DELAY = 25;
constexpr int REPEAT_INTERVAL = 4;

int indexOf(const std::string &iso) {
	for (int i = 0; i < COUNTRY_COUNT; i++) {
		if (iso == COUNTRIES[i].iso) {
			return i;
		}
	}
	return -1;
}
} // namespace

CountryPicker::CountryPicker() {
	// The language's region is the best guess at where the phone number is from.
	const std::string language = Config::getInstance().getLanguage();
	selectedIndex = indexOf(language.substr(language.find('_') + 1));
	if (selectedIndex < 0) {
		selectedIndex = indexOf("US");
	}
}

const Country &CountryPicker::selected() const { return COUNTRIES[selectedIndex]; }

void CountryPicker::show() {
	open = true;
	cursor = selectedIndex;
	repeatTimer = 0;
	dragging = false;
	draggingScrollbar = false;
	scrollTo((cursor - (VISIBLE_ROWS - 1) / 2) * ROW_H);
}

void CountryPicker::select(int index) {
	selectedIndex = index;
	open = false;
}

void CountryPicker::scrollTo(float target) { scroll = std::clamp(target, 0.0f, MAX_SCROLL); }

void CountryPicker::moveCursor(int delta) {
	cursor = std::clamp(cursor + delta, 0, COUNTRY_COUNT - 1);
	const float rowTop = cursor * ROW_H;
	if (rowTop < scroll) {
		scrollTo(rowTop);
	} else if (rowTop + ROW_H > scroll + LIST_H) {
		scrollTo(rowTop + ROW_H - LIST_H);
	}
}

void CountryPicker::update() {
	const u32 kDown = hidKeysDown();
	const u32 kHeld = hidKeysHeld();
	const u32 kUp = hidKeysUp();

	if (kDown & KEY_B) {
		open = false;
		return;
	}
	if (kDown & KEY_A) {
		select(cursor);
		return;
	}

	const u32 directions = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
	u32 direction = kDown & directions;
	if (direction) {
		repeatTimer = REPEAT_INITIAL_DELAY;
	} else if ((kHeld & directions) && --repeatTimer <= 0) {
		direction = kHeld & directions;
		repeatTimer = REPEAT_INTERVAL;
	}
	if (direction & KEY_UP) {
		moveCursor(-1);
	} else if (direction & KEY_DOWN) {
		moveCursor(1);
	} else if (direction & KEY_LEFT) {
		moveCursor(-VISIBLE_ROWS);
	} else if (direction & KEY_RIGHT) {
		moveCursor(VISIBLE_ROWS);
	}

	touchPosition touch;
	hidTouchRead(&touch);
	if (kDown & KEY_TOUCH) {
		if (touch.px < POPUP_X || touch.px > POPUP_X + POPUP_W || touch.py < POPUP_Y || touch.py > POPUP_Y + POPUP_H) {
			open = false;
			return;
		}
		dragging = touch.py >= LIST_TOP && touch.py < LIST_BOTTOM;
		draggingScrollbar = dragging && touch.px >= POPUP_X + POPUP_W - SCROLLBAR_TOUCH_W;
		tapCandidate = dragging && !draggingScrollbar;
		touchStartY = touch.py;
		lastTouchY = touch.py;
		if (draggingScrollbar) {
			scrollTo((touch.py - LIST_TOP) / LIST_H * MAX_SCROLL);
		}
	} else if (kHeld & KEY_TOUCH) {
		if (std::fabs(touch.py - touchStartY) > TAP_SLOP) {
			tapCandidate = false;
		}
		if (draggingScrollbar) {
			scrollTo((touch.py - LIST_TOP) / LIST_H * MAX_SCROLL);
		} else if (dragging) {
			scrollTo(scroll - (touch.py - lastTouchY));
		}
		lastTouchY = touch.py;
	} else if (kUp & KEY_TOUCH) {
		if (tapCandidate) {
			const int index = (int)((touchStartY - LIST_TOP + scroll) / ROW_H);
			if (index >= 0 && index < COUNTRY_COUNT) {
				select(index);
			}
		}
		dragging = false;
		draggingScrollbar = false;
		tapCandidate = false;
	}
}

void CountryPicker::render() {
	const u32 background = ScreenManager::colorBackground();

	drawOverlay(0.4f);
	drawPopupBackground(POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 0.5f, POPUP_RADIUS);

	const float rowRight = POPUP_X + POPUP_W - 16.0f;
	const int first = (int)(scroll / ROW_H);
	const int last = std::min(COUNTRY_COUNT - 1, (int)((scroll + LIST_H) / ROW_H));
	for (int i = first; i <= last; i++) {
		const float y = LIST_TOP + i * ROW_H - scroll;
		const bool isCursor = i == cursor;
		drawPopupMenuItem(POPUP_X + 5.0f, y + 1.0f, rowRight - POPUP_X - 5.0f, ROW_H - 2.0f, 0.55f, isCursor,
		                  ScreenManager::colorSelection());

		const std::string code = std::string("+") + COUNTRIES[i].dialCode;
		const float codeW = measureText(code, 0.45f, 0.45f);
		const float nameX = POPUP_X + 12.0f;
		const float codeX = rowRight - 6.0f - codeW;
		const u32 nameColor = isCursor ? ScreenManager::colorWhite() : ScreenManager::colorText();
		const u32 codeColor = isCursor ? ScreenManager::colorWhite() : ScreenManager::colorTextMuted();
		drawText(codeX, y + 5.0f, 0.6f, 0.45f, 0.45f, codeColor, code);
		drawText(nameX, y + 5.0f, 0.6f, 0.45f, 0.45f, nameColor,
		         getTruncatedText(COUNTRIES[i].name, codeX - nameX - 8.0f, 0.45f, 0.45f));
	}

	// Rows scroll underneath the header and footer, which are painted back over them.
	drawRoundedRect(POPUP_X, POPUP_Y, 0.65f, POPUP_W, HEADER_H, POPUP_RADIUS, background);
	C2D_DrawRectSolid(POPUP_X, POPUP_Y + POPUP_RADIUS, 0.65f, POPUP_W, HEADER_H - POPUP_RADIUS, background);
	drawRoundedRect(POPUP_X, LIST_BOTTOM, 0.65f, POPUP_W, FOOTER_H, POPUP_RADIUS, background);
	C2D_DrawRectSolid(POPUP_X, LIST_BOTTOM, 0.65f, POPUP_W, FOOTER_H - POPUP_RADIUS, background);

	drawCenteredText(POPUP_Y + 10.0f, 0.7f, 0.55f, 0.55f, ScreenManager::colorText(), TR("login.country.title"),
	                 BOTTOM_SCREEN_WIDTH);
	C2D_DrawRectSolid(POPUP_X + 10.0f, LIST_TOP - 3.0f, 0.7f, POPUP_W - 20.0f, 1.0f, ScreenManager::colorSeparator());

	const float thumbH = std::max(10.0f, LIST_H * LIST_H / (COUNTRY_COUNT * ROW_H));
	const float thumbY = LIST_TOP + (LIST_H - thumbH) * (scroll / MAX_SCROLL);
	drawRoundedRect(POPUP_X + POPUP_W - 10.0f, thumbY, 0.7f, 4.0f, thumbH, 2.0f, ScreenManager::colorTextMuted());

	drawCenteredText(LIST_BOTTOM + 7.0f, 0.7f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
	                 "\uE000: " + TR("common.select") + "  \uE001: " + TR("common.cancel"), BOTTOM_SCREEN_WIDTH);
}

} // namespace UI
