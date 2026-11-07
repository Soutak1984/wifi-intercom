# Feasibility of ESP32-SIP-Voice

Reference inspected: [GeorgeBregman/ESP32-SIP-Voice](https://github.com/GeorgeBregman/ESP32-SIP-Voice), main branch. License: MIT, Copyright 2026 George Bregman. The licence allows reuse with the notice kept. This project does not copy that source. The architecture does not meet the serverless requirement, so the firmware here is original. G.711, AES-128, and SHA-256 are implemented from the published standards.

## Direct station-to-station SIP

The reference is a SIP user agent. `sip_client_initiate_call()` refuses to run unless the call is idle and `is_registered` is true. Registration becomes true only after a REGISTER 200 OK from `settings->sip_server`. INVITE packets are sent to that server address, not to the callee. The task also sends REGISTER and keepalives to the server. An incoming INVITE is answered with a SIP 200 only when the build enables automatic answer, and something still has to deliver the INVITE.

That stack cannot place a call with no registrar and no PBX. A "direct IP" string in the server field would still be a SIP server address, not a second station.

## Audio and network

The reference does implement full-duplex RTP with G.711, optional G.722 and Opus, a jitter buffer, packet-loss concealment, and an NLMS echo canceller, plus an optional esp-sr canceller on ESP32-S3 with PSRAM. The microphone and amplifier parts (INMP441 and MAX98357A, or an ES8388 codec) match this project's analogue needs.

Its network path is Wi-Fi station mode with a captive portal. It has no W5500 driver and no wired-Ethernet call path.

## What was reused

Nothing was copied from that repository. The ideas that carried over are ordinary for this hardware: I2S INMP441 plus MAX98357A, 20 ms G.711 µ-law frames, a small jitter buffer, and NLMS as a best-effort echo reducer. Signalling, discovery, paging, button policy, and the W5500 bring-up are new.

## Decision

Keep a small authenticated UDP protocol between the four stations, unicast only. Do not run Asterisk, FreePBX, or a SIP registrar. The Arduino sketch now carries that protocol on the ESP32-S3 Wi-Fi station interface, with the same static addresses. It does not use a W5500.

The Arduino sketch in this folder does not use the INMP441 or the MAX98357A. Capture and playback go through a USB Audio Class 1.0 headset adapter on the ESP32-S3 USB host port. The 8 kHz µ-law frame, jitter buffer, and NLMS canceller are unchanged.
