// Static ASCII buddy frames — one representative frame per state, lifted
// verbatim from src/buddies/cat.cpp. The Stick firmware cycles through
// many poses per state for animation; on e-ink we draw just one pose
// because partial-refresh can't carry per-frame animation gracefully.
//
// Keeping the frames as string arrays (rather than reading the Stick's
// animated code) avoids pulling in M5StickC's TFT_eSprite dependencies.
// If you want to add other species, copy the same 5-line rows out of
// their .cpp files and extend the struct below.

#pragma once

struct BuddyFrame {
  const char* lines[5];
};

namespace buddy_cat {
  static const BuddyFrame SLEEP = {{
    "            ",
    "            ",
    "   .-..-.   ",
    "  ( -.- )   ",
    "  `------`~ ",
  }};
  static const BuddyFrame IDLE = {{
    "            ",
    "   /\\_/\\    ",
    "  ( o   o ) ",
    "  (  w   )  ",
    "  (\")_(\")   ",
  }};
  static const BuddyFrame BUSY = {{
    "      .     ",
    "   /\\_/\\    ",
    "  ( o   o ) ",
    "  (  w   )/ ",
    "  (\")_(\")   ",
  }};
  static const BuddyFrame ATTENTION = {{
    "            ",
    "   /^_^\\    ",
    "  ( O   O ) ",
    "  (  v   )  ",
    "  (\")_(\")   ",
  }};
  static const BuddyFrame CELEBRATE = {{
    "    \\o/     ",
    "   /\\_/\\    ",
    "  ( ^   ^ ) ",
    " /(  W   )\\ ",
    "  (\")_(\")   ",
  }};
  // Reused for DND (a cat with its eyes closed reads fine as "do not disturb").
  static const BuddyFrame DND = {{
    "            ",
    "   /\\_/\\    ",
    "  ( -   - ) ",
    "  (  w   )  ",
    "  (\")_(\")   ",
  }};
}
