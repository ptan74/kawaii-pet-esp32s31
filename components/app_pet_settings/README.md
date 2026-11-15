# app_pet_settings

Single settings store for the pet (NVS namespace `kawaii_pet`, one versioned
blob): phone hotspot, Caplora key and endpoints, chat/voice models and voice,
animal, owner name, volume, "talk on its own", and the randomly generated
setup-network password.

Thread-safe getters return copies under a mutex. `set()` is called only by the
setup web server, so callers are serialized; the change callback runs on that
task. Secrets (`api_key`, `voice_api_key`, hotspot password) are kept only in NVS and
RAM; callers zero their copies after use. Chat defaults to Caplora
(`https://caploralab.online/api/v1`, `claude-haiku-5-5`); speech defaults to
OpenAI (`gpt-4o-mini-transcribe`, `gpt-4o-mini-tts`, voice `coral`) with its own
key, all editable on the setup page.

Blob versions: v2 appended `voice_api_key`. A v1 blob loads as a prefix, and its
old Caplora voice paths (which return 404) are replaced with the speech
defaults. Append new fields at the end and bump the version.
