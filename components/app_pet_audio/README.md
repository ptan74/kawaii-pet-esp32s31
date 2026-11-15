# app_pet_audio

Owns the ES8311 speaker and microphone. A single `pet_audio` task executes
queued requests in order: synthesized effects (16 kHz mono), reply clips
(WAV played directly, MP3 through a dedicated `esp_audio_simple_player`),
radio streams (second player instance) and recordings (16 kHz mono, up to
15 s, delivered to `rec_cb`, which owns the buffer).

Rules: recording stops music and closes the speaker (shared I2S clock);
clips interrupt music; effects are skipped while music plays. Clip playback
reports loudness about 20 times a second for lip-sync. Callbacks run on the
audio or player tasks and must not block. One instance per boot.
