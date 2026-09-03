# DaaVoiceTerminal

ESP32-S3 telephone AI box firmware.

Audio path: **phone line PCM (Si3050) → Wi-Fi → AI server (Opus) → PCM back to the line**.

This branch is stripped of the previous product stack (LCD, RFID, motor, 4G, local p3 playback, wake word, camera).

## Current bring-up skeleton

- Wi-Fi + OTA + MQTT/WebSocket protocol
- I2S duplex placeholder for the Si3050 PCM highway (`NoAudioCodecDuplex`)
- Opus encode/decode loop
- BOOT: click toggles conversation; long-press forces Wi-Fi AP config

Si3050 SPI init, ring detect, and off-hook are **not implemented yet**. PCM pin numbers in `main/boards/DaaVoiceTerminal/config.h` are placeholders.

## Build

```bash
idf.py set-target esp32s3
idf.py build
```

Target: ESP32-S3-WROOM-1 (R8N16, 16MB flash + 8MB Octal PSRAM).
