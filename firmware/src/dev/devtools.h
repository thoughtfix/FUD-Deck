// Serial remote control for development builds (FUD_DEVTOOLS).
//   key up|down|left|right|center|back|holdcenter
//   tap X Y
//   shot              screen as RGB332 rows (tools/probe.py shot reads this format)
//   state             current screen
//   post              POST report
#pragma once

namespace devtools {
void poll();
}
