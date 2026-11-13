// Zuse's face: alarms, short reactions, low battery, and idle life (glance, blink, yawn,
// flicker). Ported from FUD-Firmware (thoughtfix, GPL-3.0). Mood ideas credited there to
// Pwnagotchi and M5PORKCHOP.
#pragma once
#include <Arduino.h>

namespace zuse {
void tick(bool homeVisible);           // advance idle animations while Home is up
const char* face();                    // the face to show right now
bool idleBusy();                       // a blink, glance or yawn is playing
void holdIdle(uint32_t ms);            // no new idle moves for ms (Home's own animations)
void react(const char* faceName, uint32_t ms = 4000);   // short reaction (passive hit, mute)
void toast(const char* msg, uint32_t ms = 1300);
const char* toastText();               // nullptr when none
// Speech bubble: Zuse's occasional remark on Home (a few seconds, two short lines max).
void say(const char* text, uint32_t ms = 5000);
const char* sayText();                 // nullptr when none
}
