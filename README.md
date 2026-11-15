# Kawaii Pet for ESP-Mosaico

A round, kawaii animated pet (dog, cat, rabbit, hamster, panda or penguin)
that you talk to. Hold the mic button, speak, and it answers out loud through
Caplora Lab models, with moods (happy, laughing, shy, angry, sad, scared,
in love...), animal sounds, feeding, napping with snoring, and radio music on
request. On first launch it asks your name and uses it from then on.

Setup from your phone: tap Setup on the pet and follow the steps. Join the
pet's `KawaiiPet-XXXX` Wi-Fi with the password shown, open
http://192.168.4.1, and enter your phone hotspot, Caplora key, chat model,
voice settings and animal.

Host tests: `cc -I components/app_pet_core/include tests/host/test_pet_text.c components/app_pet_core/pet_text.c components/app_pet_core/pet_types.c`
(and `test_pet_life_synth.c` with `pet_life.c pet_synth.c pet_types.c -lm`).

Created with `python mosaico.py project init my_app`.
The same GSP 1.5.1 scene and portable C UI run on PC and ESP-Mosaico.

From the workspace root:

```sh
python mosaico.py project sim --project projects/kawaii_pet --interactive
python mosaico.py recover
python mosaico.py iris system-update --project projects/kawaii_pet
```

Run `recover` before the first install on a blank or unverified device.
Use `system-update` for new apps or changed resources/layouts. Use `app-update`
only for code changes with an identical full partition table and resources.

Application components:

- `components/app_pet_core`: portable pet logic: artwork renderer, life simulation, text cleanup, sound synthesis.
- `components/app_pet_ui`: GSP screen controller; `host/` is the simulator adapter with a scripted fake Caplora.
- `components/app_pet_settings`: NVS settings (key, models, voice, animal, name, hotspot).
- `components/app_pet_net`: setup Wi-Fi, hotspot connection and the phone setup page.
- `components/app_pet_audio`: speaker/mic owner: effects, voice clips, radio, recording.
- `components/app_pet_brain`: Caplora client: speech-to-text, chat, text-to-speech.
- `components/app_stations`: Singapore radio stations (shared with sg_fm_radio).
- `components/app_display`: BSP-to-GSP display and touch policy.
- `main`: device startup and wiring only.

`project sim` discovers a unique scene under `components/*/ui/main.json` (or the
older root `ui/main.json`). With multiple scenes, select one using `--scene PATH`,
relative to the project. This previews only that scene; omit `--scene` to run the
PC backend and its compiled application behavior.

Dependency direction: `main` composes every `app_*` component; `app_pet_ui`,
`app_pet_audio`, `app_pet_brain` and `app_pet_settings` depend on `app_pet_core`;
`app_pet_net` and `app_pet_brain` read `app_pet_settings`. No component depends on
`main` or on a sibling's private state; only `main` routes UI <-> audio <-> brain. Public headers live in `include/`; private state stays
in implementation files. Each component README includes its contract and usage.

When adding behavior, keep `main` as a composition root. Add cohesive `app_*`
components only for independent responsibilities, declare public/private CMake
dependencies explicitly, and keep the graph acyclic. Avoid cross-module globals;
pass handles, context and value snapshots. Keep each public API comment within
three lines, describing its purpose and key preconditions/results. Put detailed
ownership, lifecycle, threading and timeout contracts in the component README;
document units and ranges beside fields.
Third-party dependencies retain upstream names. Pins and workspace overrides live
in `main/idf_component.yml` and the root CMake configuration. Apply local patches
in a dedicated dependency checkout and select it with `override_path`; never edit
`managed_components/` in place.

Git, editor and hook entry files stay at the root; format rules, project policy
and setup instructions live in `tools/quality/`. See its README to enable hooks.
The `tools/quality/policy.json` prefix rule checks local application component names.
Architecture and API semantics also require code review; formatter/static-analysis
success alone does not establish those contracts.
