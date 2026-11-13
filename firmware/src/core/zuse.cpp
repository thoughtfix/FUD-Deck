#include "zuse.h"
#include "../board/power.h"
#include "detections.h"

namespace zuse {

struct Step {
  const char* f;
  uint16_t ms;
};
static const Step A_GLANCE[] = {{"glance-left", 800}, {"defaultsmile", 250}, {"glance-right", 800}, {"defaultsmile", 1}};
static const Step A_BLINK[] = {{"winkingface", 300}, {"defaultsmile", 1}};
static const Step A_YAWN[] = {{"yawning-face", 1600}, {"defaultsmile", 1}};
static const Step A_FLICK[] = {{"total-blank-face", 120}, {"defaultsmile", 1}};
#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const Step* anim = nullptr;
static int animN = 0, animI = 0;
static uint32_t animUntil = 0, animNext = 6000;
static const char* animFace = nullptr;
static const char* reactFace = nullptr;
static uint32_t reactUntil = 0;
static const char* toastMsg = nullptr;
static uint32_t toastUntil = 0;

static void play(const Step* a, int n) { anim = a; animN = n; animI = 0; animUntil = 0; }

static void service() {
  if (!anim) { animFace = nullptr; return; }
  uint32_t now = millis();
  if (now < animUntil) return;
  if (animI >= animN) { anim = nullptr; animFace = nullptr; return; }
  animFace = anim[animI].f;
  animUntil = now + anim[animI].ms;
  animI++;
}

void tick(bool homeVisible) {
  if (!homeVisible || det::alarmActive()) return;
  service();
  uint32_t now = millis();
  if (!anim && now > animNext) {
    if (now - det::lastDetectionMs() > 5UL * 60 * 1000) play(A_YAWN, NELEM(A_YAWN));   // bored
    else {
      int pick = esp_random() % 4;   // millis() locks into a cycle with the 6-12 s gap
      if (pick == 0) play(A_GLANCE, NELEM(A_GLANCE));
      else if (pick == 1) play(A_FLICK, NELEM(A_FLICK));
      else play(A_BLINK, NELEM(A_BLINK));
    }
    animNext = now + 6000 + esp_random() % 6000;  // 6-12 s
  }
}

const char* face() {
  uint32_t now = millis();
  if (det::alarmActive()) {
    int i = det::activeAttackIdx();
    return (i >= 0 && det::at(i).cat == det::HACKDEV) ? "alarm-hacker" : "alarm-attack";
  }
  if (reactFace && now < reactUntil) return reactFace;
  int bat = power::percent();
  if (bat >= 0 && bat < 15 && !power::charging()) return "sadface";
  if (animFace) return animFace;
  return "defaultsmile";
}

bool idleBusy() { return anim != nullptr; }
void holdIdle(uint32_t ms) {
  uint32_t until = millis() + ms;
  if ((int32_t)(until - animNext) > 0) animNext = until;
}

void react(const char* f, uint32_t ms) { reactFace = f; reactUntil = millis() + ms; }
void toast(const char* m, uint32_t ms) { toastMsg = m; toastUntil = millis() + ms; }
const char* toastText() { return (toastMsg && millis() < toastUntil) ? toastMsg : nullptr; }

static char sayBuf[80];
static uint32_t sayUntil = 0;
void say(const char* t, uint32_t ms) {
  strncpy(sayBuf, t, sizeof(sayBuf) - 1);
  sayBuf[sizeof(sayBuf) - 1] = 0;
  sayUntil = millis() + ms;
}
const char* sayText() { return millis() < sayUntil ? sayBuf : nullptr; }

}  // namespace zuse
