# app_pet_core

Portable pet logic shared by the device and the GSP simulator; no ESP-IDF or
RTOS dependencies. All functions are pure or operate on caller-owned state.

| Module | Responsibility |
| --- | --- |
| `pet_types` | Animals (dog, cat, rabbit, hamster, panda, penguin), 16 moods, sound ids, names and foods. |
| `pet_text` | Cleans model replies into plain sentences (drops Markdown, emoji, links, stray quotes), parses `[mood:x]`/`[action:y]` tags, pages text for the bubble, extracts the owner's name, detects music/sleep intents. |
| `pet_render` | Procedural kawaii artwork: builds up to 128 anti-aliased primitives per frame and rasterizes any region of the 240x240 RGB565 canvas. |
| `pet_life` | Needs (hunger, energy, loneliness), autonomous moods, blinking, eating, napping with snoring, spontaneous lines that use the owner's name. |
| `pet_synth` | 16 kHz procedural sounds: barks, meows, squeaks, honks, panting, purring, growls, hisses, whimpers, crunching, snoring, yawns, giggles. |

Threading: no internal locking. `pet_life_t` and `pet_scene_t` have a single
owner (the UI render task). `pet_text_clean`/`pet_reply_parse` use about
2 KiB of stack.

Tests (host): `tests/host/test_pet_text.c`, `tests/host/test_pet_life_synth.c`;
`tests/host/pet_contact_sheet.c` renders every animal and mood to a PPM.
