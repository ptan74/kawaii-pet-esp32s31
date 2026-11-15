# app_pet_brain

Caplora Lab chat plus an OpenAI-compatible speech service, on its own task
(12 KiB stack in PSRAM). Chat uses the Caplora key; speech uses
`voice_api_key` (falls back to the Caplora key), because Caplora keys carry no
speech models (checked against `/api/v1/models` on 2026-11-12). Defaults are
OpenAI `gpt-4o-mini-transcribe` and `gpt-4o-mini-tts` (voice `coral`, with an
animal-themed speaking style). Jobs are queued:

1. **Listen**: WAV-wrap the recording and POST multipart to the
   speech-to-text URL (`file`, `model`, `language`); expects `{"text": ...}`.
2. **Chat** (chat recordings only): POST `{base}/chat/completions` with a pet
   persona system prompt (animal, owner name, mood/action tag rules, station
   list) and the last 8 messages; reads `choices[0].message.content`.
3. **Speak**: POST `{"model","voice","input","response_format":"mp3"}` to the
   text-to-speech URL and hand the audio to `voice`.

Requests wait for the `online` callback: before the network is up (when even the
TCP/IP stack may not exist) chats report "no internet" and background lines
are skipped. Failures become plain sentences through `error` (no internet, key rejected,
out of credit, endpoint not found, busy). Background `say()` lines fail
silently. Settings are read per request; the key copy is zeroed after each
job. TLS uses the ESP-IDF certificate bundle. All speech URLs and models are
editable on the setup page.
