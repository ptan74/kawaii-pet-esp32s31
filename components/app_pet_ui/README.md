# app_pet_ui

GSP controller for the 480x480 pet screen (`ui/main.json`): status row, speech
bubble, the 240x240 pet canvas drawn by `pet_render`, a touch area for
petting, and Feed / Sleep / hold-to-talk / Music / Setup buttons plus the
setup panel. Runs unchanged on the device and in the simulator
(`host/platform_pc.c` scripts a fake microphone, Caplora and speaker).

**Flow.** With no saved name the pet asks for it; the next recording is a
`APP_PET_TALK_NAME` capture and the extracted name is saved through
`sink.save_name`. Afterwards each hold of the mic button is a chat: LISTEN
mood while held, THINK until the reply, then the reply's mood, actions
(music, stop, feed, sleep) and paged bubble text while `SPEAKING`.

**Threading.** All controller state belongs to the GSP render task (event,
50 ms frame timer and canvas draw callback). Sink callbacks run there and
must only queue work. The `app_pet_ui_*` input functions are the only
cross-task entry points: they copy into an inbox under the platform lock
(`config.lock`), drained once per frame.

**Lifetime.** `create` registers the canvas draw callback, frame timer and
event handler; `destroy` (after quiescing dispatch) detaches them. On the
device the controller lives until reboot.

The canvas is invalidated in full every frame: partial invalidation leaves
stale columns in the pinned simulator, and the raster is cheap.

Dependencies: `esp-gsp`, `app_pet_core`.
