# app_pet_net

Wi-Fi in AP+STA mode plus the phone setup page.

- **Setup network** `KawaiiPet-XXXX` (WPA2, per-device password shown on the
  Setup screen), IP 192.168.4.1.
- **Station** joins the phone hotspot saved on the setup page, or falls back
  to Vibe Mode's saved network (read-only `sysmeta`/`shared`). Reconnects with
  1-30 s backoff. Credentials use `WIFI_STORAGE_RAM`.
- **Web page** (`setup_web.c`, port 80 on both interfaces): `GET /` form,
  `POST /save` (urlencoded, max 3 KiB). The API key and passwords are never
  echoed; blank fields keep saved values; all values are HTML-escaped.

Status changes are reported through the callback on the event or httpd task.
One instance per boot; no stop.
