# Testing

Run the host tests first. Then test two physical stations. Expand to four only after hangup, microphone control, and auto-answer behave on the pair.

## Host suite

`tests/test_core.c` drives four in-process stations and a loopback queue. It does not open a socket and it does not claim acoustic results. The cases it encodes:

- SHA-256, HMAC-SHA256, and AES-128 known answers
- µ-law zero and a round trip
- jitter prime, playout, and concealment
- NLMS attenuation of a 40-sample delayed tone
- idle Enable Mic arms but does not transmit
- 100 calls 101, auto-answer, caller can transmit, callee microphone stays off until its own Talk control
- mute flag produces silence, End Call returns both sides to idle and rejects late media
- 101 calls 100 without arming the callee microphone
- every ordered pair among the four stations
- own-station select places no call
- offline destination places no call
- an invite that is delivered only after the 3 s give-up does not restore the call
- a busy station rejects, and the existing call stays up
- simultaneous invites (glare) settle on one session, the lower id wins
- replayed media and a replayed invite do not recreate a call
- a short packet, a bad HMAC, and a station id outside 100–103 are rejected
- a non-reception page is refused
- page-all plays on the floors, floor microphones stay off, and Enable Mic on a floor does not transmit
- a page does not take over an existing private call
- a single-floor page reaches only that floor
- link down clears the caller even if the BYE is never delivered; the other side times out and disarms
- a new boot id ends the far side's call
- a two-minute keepalive soak keeps a single session

Build and run:

```
cd tests
make test
```

### Host results

| Check | Result |
| --- | --- |
| `make test` | Not run. The command runner failed with Windows error 216 before gcc or clang started. |

Do not mark this row passed until the process prints a pass count and exits 0.

## Bench procedure

Provision two boards as 100 and 101 using [NETWORK.md](NETWORK.md). Use `audiotest` on each board while idle and confirm the local speaker sounds. Use `miclevel` while speaking and confirm the RMS moves and `mic_tx` stays 0.

Then:

1. 100 calls 101. 101 accepts with no extra tap. Both speakers are on. Both microphones stay off. 101's web page says the microphone is off, not live.
2. 101 calls 100 after End. Same speaker and microphone result.
3. On a connected call, Talk on 100 sets only 100's `mic_tx`, and 101 hears 100. Tap Mute: `mic_tx` goes to 0 and 101 hears silence, while 101's speaker stays enabled.
4. End on 100. Both stations return to idle, both microphones are off, and a second End does not place a packet that brings the call back. Confirm on the far station too.
5. After hangup, `status` shows `mic_armed 0` on both.
6. Unplug 101 and call it from 100. 100 reports offline and stays idle.
7. Call 100 from 101, then from a third station call the busy one. The third station reports busy and the original pair stays connected.
8. Press Call on both stations, aimed at each other, as close together as possible. One call remains. `status` shows one remote and one session.
9. From 100, page 101 while 101 is idle. 101 plays audio, its microphone stays off, and Mic on 101 does not let 100 hear 101.
10. Call 101 from 102, then page all from 100. 101 and 102 stay in their call. 103 plays the page if it is idle. 103's microphone stays off.
11. Drop Wi-Fi during a call (power off the access point, or move the station out of range). The station that lost Wi-Fi goes idle, microphone off, Wi-Fi bars empty. The other station clears within about 5 seconds.
12. From a PC, send a short UDP datagram to port 47000 and a random 100-byte datagram. `status` increases `malformed` or `bad_hmac` and no call starts. A packet with the wrong key must not open a microphone.

With four boards, repeat a call from every station to every other station (12 directions). Record the result in the table below. The firmware's host test is the stand-in until those calls are done; it is not a substitute for the acoustic test.

## Measurements to record

Fill this table from the bench. Leave a cell blank rather than estimating.

| Measurement | How | Result |
| --- | --- | --- |
| One-way speech delay | Clap or impulse, record both rooms, measure the gap | Not measured |
| Echo with canceller | Talk in a quiet room, note if the far end returns a strong copy | Not measured |
| Packet loss | `status` media counters over a 10 minute call | Not measured |
| Free heap | `status` heap line while idle and while in a call | Not measured |
| Soak | Two stations in a call for at least 1 hour, then End Call | Not measured |
| Microphone live vs transmit | Live only while the far end hears audio | Not measured |

The architecture note's 120–160 ms figure is a scheduling budget (20 ms frame, 80 ms jitter target, DMA). It is not a measured delay.

`status` also prints `plc`, `media_drop`, `usb_rx`, and `usb_tx`. Those counters are the ones to copy into the table after a real call.
