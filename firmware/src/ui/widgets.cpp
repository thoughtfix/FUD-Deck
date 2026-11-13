#include "widgets.h"
#include "../board/power.h"

using namespace theme;

namespace widgets {

static const int BACK_W = 64;

// Battery corner. On USB the icon turns green with a white bolt: percent while charging,
// FULL once the charger says it is done.
void headerBattery() {
  const int x = W - 62, y = 6;
  tft.fillRect(x - 2, 0, W - x + 2, HEADER_H - 1, BG);
  int pct = power::percent();
  bool usb = power::usb();
  bool chg = power::charging();
  uint16_t c = usb ? GOOD : (pct >= 0 && pct < 15) ? ALERT : FG;
  tft.drawRect(x, y, 24, 14, c);
  tft.fillRect(x + 24, y + 4, 3, 6, c);
  int fill = usb && !chg ? 100 : pct;
  if (fill > 0) tft.fillRect(x + 2, y + 2, (20 * fill) / 100, 10, c);
  if (usb) {
    // bolt: upper and lower halves, outlined in black so it shows on any fill
    const int bx = x + 12, by = y + 7;
    for (int o = 0; o < 2; o++) {
      uint16_t bc = o ? 0xFFFF : BG;
      int g = o ? 0 : 1;
      tft.fillTriangle(bx + 2 + g, by - 7 - g, bx - 4 - g, by + 1 + g, bx + 1 + g, by + 1 + g, bc);
      tft.fillTriangle(bx - 1 - g, by - 1 - g, bx + 4 + g, by - 1 - g, bx - 2 - g, by + 7 + g, bc);
    }
  }
  tft.setTextSize(1);
  tft.setTextColor(c, BG);
  tft.setTextDatum(textdatum_t::middle_left);
  char b[8];
  if (usb && !chg) snprintf(b, sizeof(b), "FULL");
  else if (pct < 0) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%d%%", pct);
  tft.drawString(b, x + 30, y + 7);
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
  tft.fillRoundRect(3, y + 2, W - 6, lv.rowH - 4, 4, bg);
  tft.setTextSize(2);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.drawString(buf, 10, y + lv.rowH / 2);
  tft.setTextDatum(textdatum_t::top_left);
}

void listDraw(ListView& lv, int n, bool full) {
  if (lv.sel >= n) lv.sel = n > 0 ? n - 1 : 0;
  if (lv.sel < lv.top) lv.top = lv.sel;
  if (lv.sel >= lv.top + lv.rows) lv.top = lv.sel - lv.rows + 1;
  if (full || lv.top != lv.lastTop) {
    for (int r = 0; r < lv.rows; r++) drawRow(lv, n, lv.top + r);
    // scroll hints
    tft.fillRect(W - 10, lv.y0, 10, lv.rows * lv.rowH, BG);
    if (lv.top > 0) tft.fillTriangle(W - 9, lv.y0 + 8, W - 1, lv.y0 + 8, W - 5, lv.y0 + 2, DIM);
    if (lv.top + lv.rows < n) {
      int yb = lv.y0 + lv.rows * lv.rowH;
      tft.fillTriangle(W - 9, yb - 8, W - 1, yb - 8, W - 5, yb - 2, DIM);
    }
  } else if (lv.sel != lv.lastSel) {
    drawRow(lv, n, lv.lastSel);
    drawRow(lv, n, lv.sel);
  }
  lv.lastSel = lv.sel;
  lv.lastTop = lv.top;
}

void listMove(ListView& lv, int n, int delta) {
  if (n <= 0) return;
  lv.sel = (lv.sel + delta + n) % n;  // wraps, like FUD
}

int listHit(ListView& lv, int n, int x, int y) {
  (void)x;
  if (y < lv.y0) return -1;
  int r = (y - lv.y0) / lv.rowH;
  if (r >= lv.rows) return -1;
  int i = lv.top + r;
  return i < n ? i : -1;
}

}  // namespace widgets
