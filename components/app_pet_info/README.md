# app_pet_info

Network time and local weather for the clock screen.

- **Time:** SNTP (`pool.ntp.org`). Local time = UTC + `app_pet_info_utc_offset()`.
- **Location:** IP geolocation (ip-api.com, no key).
- **Weather:** Open-Meteo current conditions (no key), e.g. "Singapore 31C Partly cloudy".

One task refreshes every 15 minutes (60 s retry on failure) using PSRAM buffers.
The callback runs on that task and must not block.
