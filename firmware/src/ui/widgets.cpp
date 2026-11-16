#include "widgets.h"
#include "../board/power.h"

using namespace theme;

namespace widgets {

static const int BACK_W = 64;

// Battery corner. The gauge on this hardware is rough and not trustworthy, so we do not
// pretend to a precise percent. We show only the two states the board reads reliably: a
// green bolt for CHG/FULL, and a red LOW mark near empty. In between, a dim "~" says the
// level is approximate. See the note in Settings > About.
void headerBattery() {
  const int x = W - 62, y = 6;
  tft.fillRect(x - 2, 0, W - x + 2, HEADER_H - 1, BG);
  int pct = power::percent();
  bool usb = power::usb();
  bool chg = power::charging();
  bool low = !usb && pct >= 0 && pct < 20;
  // Green on USB (reliable), red when low (reliable), dim otherwise (do not trust the number).
  uint16_t c = usb ? GOOD : low ? ALERT : DIM;
  tft.drawRect(x, y, 24, 14, c);
  tft.fillRect(x + 24, y + 4, 3, 6, c);
  if (usb) {
    tft.fillRect(x + 2, y + 2, 20, 10, c);   // solid while on external power
    // bolt: upper and lower halves, outlined in black so it shows on any fill
    const int bx = x + 12, by = y + 7;
    for (int o = 0; o < 2; o++) {
      uint16_t bc = o ? 0xFFFF : BG;
      int g = o ? 0 : 1;
      tft.fillTriangle(bx + 2 + g, by - 7 - g, bx - 4 - g, by + 1 + g, bx + 1 + g, by + 1 + g, bc);
      tft.fillTriangle(bx - 1 - g, by - 1 - g, bx + 4 + g, by - 1 - g, bx - 2 - g, by + 7 + g, bc);
    }
  } else if (low) {
    // exclamation inside the icon: the one reading worth trusting on battery
    tft.fillRect(x + 11, y + 3, 3, 5, c);
    tft.fillRect(x + 11, y + 9, 3, 2, c);
  } else {
    tft.fillRect(x + 4, y + 6, 16, 2, c);    // a dash: present, level approximate
  }
  tft.setTextSize(1);
  tft.setTextColor(c, BG);
  tft.setTextDatum(textdatum_t::middle_left);
  const char* t = usb ? (chg ? "CHG" : "FULL") : low ? "LOW" : "~";
  tft.drawString(t, x + 30, y + 7);
  tft.setTextDatum(textdatum_t::top_left);
}

void header(const char* title, bool showBack, bool showHamburger) {
  tft.fillRect(0, 0, W, HEADER_H, BG);
  tft.drawFastHLine(0, HEADER_H - 1, W, FG);
  tft.setTextColor(FG, BG);
  tft.setTextSize(2);
  int tx = 6;
  if (showBack) {
    tft.drawRoundRect(2, 3, BACK_W - 4, HEADER_H - 7, 4, FG);
    tft.setTextSize(1);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString("< BACK", BACK_W / 2, HEADER_H / 2 - 1);
    tft.setTextSize(2);
    tx = BACK_W + 4;
  } else if (showHamburger) {
    for (int i = 0; i < 3; i++) tft.fillRoundRect(5, 7 + i * 6, 18, 3, 1, FG);
    tx = 30;
  }
  // Shrink long titles so they never run into the battery corner.
  const int room = (W - 66) - tx;
  float size = 2.0f;
  int len = strlen(title);
  if (len * 12 > room) size = max(1.0f, (float)room / (len * 6));
  tft.setTextSize(size);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.drawString(title, tx, HEADER_H / 2);
  tft.setTextDatum(textdatum_t::top_left);
  headerBattery();
}

bool hitBack(int x, int y) { return x < BACK_W + 8 && y < HEADER_H + 8; }
bool hitHamburger(int x, int y) { return x < 36 && y < HEADER_H + 8; }

static const char* lastHint = "";
static void footerText(const char* t) {
  tft.fillRect(0, FOOTER_Y - 2, W, H - FOOTER_Y + 2, BG);
  tft.drawFastHLine(0, FOOTER_Y - 2, W, DIM);
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(t, W / 2, FOOTER_Y + 7);
  tft.setTextDatum(textdatum_t::top_left);
}
void footer(const char* hint) { lastHint = hint; footerText(hint); }
void footerFlash(const char* msg) { footerText(msg); }
void footerRestore() { footerText(lastHint); }

int drawWrapped(const char* s, int x, int y, int w, uint16_t color, uint16_t bg) {
  const int cw = 6, lh = 11, maxc = w / cw;
  tft.setTextSize(1);
  tft.setTextColor(color, bg);
  char line[64];
  while (*s) {
    int take = 0, lastSpace = -1;
    while (s[take] && s[take] != '\n' && take < maxc) {
      if (s[take] == ' ') lastSpace = take;
      take++;
    }
    if (s[take] && s[take] != '\n' && lastSpace > 0) take = lastSpace;
    snprintf(line, sizeof(line), "%.*s", take, s);
    tft.drawString(line, x, y);
    y += lh;
    s += take;
    if (*s == '\n') s++;
    else while (*s == ' ') s++;
  }
  return y;
}

void clearBody() { tft.fillRect(0, HEADER_H, W, FOOTER_Y - 2 - HEADER_H, BG); }

// Scrollbar geometry, shared by the row, the bar, and the hit test so they always agree.
// SB_RAIL is the right strip reserved for the bar: rows stop short of it and a tap inside it
// scrolls instead of selecting a row. SB_W is the visible bar, wide enough to aim at.
static const int SB_RAIL = 26;
static const int SB_W = 10;

static void drawRow(ListView& lv, int n, int i) {
  int r = i - lv.top;
  if (r < 0 || r >= lv.rows) return;
  int y = lv.y0 + r * lv.rowH;
  bool sel = i == lv.sel;
  if (i >= n) {
    tft.fillRect(0, y, W, lv.rowH, BG);
    return;
  }
  char buf[48] = {0};
  uint16_t color = FG;
  if (lv.label) lv.label(i, buf, sizeof(buf), &color);
  uint16_t bg = sel ? color : BG, fg = sel ? BG : color;
  tft.fillRect(0, y, W, lv.rowH, BG);
  tft.fillRoundRect(3, y + 2, W - 3 - SB_RAIL, lv.rowH - 4, 4, bg);   // stops short of the scrollbar rail
  tft.setTextSize(2);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.drawString(buf, 10, y + lv.rowH / 2);
  tft.setTextDatum(textdatum_t::top_left);
}

// A visible scrollbar in the right-hand lane: a track the height of the list and a thumb
// sized and placed by how far down the list we are. Drawn after the rows so a row repaint
// never leaves a stale thumb behind. No bar when the whole list fits.
static void drawScrollbar(ListView& lv, int n) {
  const int w = SB_W, x = W - w - 3;
  const int y0 = lv.y0, h = lv.rows * lv.rowH;
  tft.fillRect(W - SB_RAIL, y0, SB_RAIL, h, BG);     // clear the whole rail (old thumb included)
  if (n <= lv.rows) return;
  tft.drawRoundRect(x, y0, w, h, 3, DIM);            // track
  int thumbH = max(18, h * lv.rows / n);
  int thumbY = y0 + (h - thumbH) * lv.top / (n - lv.rows);
  tft.fillRoundRect(x, thumbY, w, thumbH, 3, FG);    // thumb
}

void listDraw(ListView& lv, int n, bool full) {
  if (lv.sel >= n) lv.sel = n > 0 ? n - 1 : 0;
  if (lv.sel < lv.top) lv.top = lv.sel;
  if (lv.sel >= lv.top + lv.rows) lv.top = lv.sel - lv.rows + 1;
  if (full || lv.top != lv.lastTop) {
    for (int r = 0; r < lv.rows; r++) drawRow(lv, n, lv.top + r);
  } else if (lv.sel != lv.lastSel) {
    drawRow(lv, n, lv.lastSel);
    drawRow(lv, n, lv.sel);
  }
  drawScrollbar(lv, n);
  lv.lastSel = lv.sel;
  lv.lastTop = lv.top;
}

void listMove(ListView& lv, int n, int delta) {
  if (n <= 0) return;
  lv.sel = (lv.sel + delta + n) % n;  // wraps, like FUD
}

int listHit(ListView& lv, int n, int x, int y) {
  int h = lv.rows * lv.rowH;
  if (y < lv.y0 || y >= lv.y0 + h) return -1;
  // Tap in the scrollbar rail: jump the list so this point maps to that spot in it, bottom
  // of the bar = bottom of the list. It scrolls, it does not pick a row. Returns -1.
  if (n > lv.rows && x >= W - SB_RAIL) {
    int maxTop = n - lv.rows;
    int top = (y - lv.y0) * maxTop / (h - 1);
    if (top < 0) top = 0;
    if (top > maxTop) top = maxTop;
    lv.top = top;
    lv.sel = top;
    listDraw(lv, n, true);
    return -1;
  }
  int r = (y - lv.y0) / lv.rowH;
  if (r >= lv.rows) return -1;
  int i = lv.top + r;
  return i < n ? i : -1;
}

}  // namespace widgets
