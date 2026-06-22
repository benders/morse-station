# Field Notes — 2026-06-19

Outdoor field test of the fox-hunt system (fox + hunters + instructor remote
control). Raw observations first, then proposed fixes and open questions for
each. Nothing here is implemented yet — this is the post-exercise writeup that
feeds `TODO.md`.

---

## 1. Range vs. power setting

**Observed.** Outdoor range is good on **Low** power. For cross-camp hunting we
will likely run **Med**, stepping to **High** when the fox is placed inside the
lodge building (walls eat the link budget).

**Assessment.** This matches the link-margin behavior we already see on the
bench: RSSI saturates at Med/High at short range, so Low is the right default
for keeping a usable volume/strength gradient close in, and the higher steps are
there to buy building penetration and cross-camp distance. No change needed to
the power *levels*; this is an operating-procedure note.

**Action / procedure.**
- Default the fox to **Low** for open-field play.
- Document a simple siting rule: *open ground → Low/Med; fox indoors or across
  the whole camp → High.*
- Keep §15.249 in mind when running High with the V4 FEM PA engaged — log the
  intended EIRP per siting (the per-board EIRP calc is still an open TODO).

---

## 2. RSSI meter is hard to use for ranging

**Observed.** Using the on-screen RSSI bar to judge distance is difficult. Two
things to revisit: (a) the perceptual strength range (does the bar's span match
what a hunter actually experiences walking in?), and (b) short-time averaging,
because the bar jumps packet-to-packet.

**Root cause.** The meter today is a single-sample, linear map:
`display.cpp rssi_to_px()` clamps RSSI to **−110 … −40 dBm** and scales that
linearly to the 0–128 px bar, redrawn on every received packet with **no
smoothing**. Two problems:
- **No temporal averaging.** Each packet's instantaneous RSSI drives the bar, so
  fading and per-packet variance make it twitch. Hard to read as "warmer /
  colder."
- **Span/curve probably wrong for the working range.** A linear-in-dBm bar over
  a 70 dB window spends most of its travel in regions the hunter rarely sits in.
  Near the fox the bar pins near full; at the useful hunting distance small,
  meaningful changes barely move it.

**Proposed fixes.**
- **Short-time averaging.** Maintain an exponential moving average of RSSI
  (e.g. `ema = ema + α·(rssi − ema)`, α ≈ 0.2–0.3) and drive the bar from the
  EMA, not the raw sample. Cheap, no extra state to speak of. Optionally show a
  faint "instantaneous" tick over the smoothed fill.
- **Re-fit the perceptual range.** Re-measure the RSSI actually seen at Low
  power across the distances we play at, then either (a) tighten the clamp window
  to that working band, or (b) apply a mild non-linear (e.g. piecewise or
  gamma) map so the bar's resolution lands where hunters operate. Note that RSSI
  varies per device/antenna (see §3), so calibrate against the *gradient*, not
  absolute dBm.
- **Consider segmented bars** (5–7 chunky blocks) instead of a smooth fill — a
  coarse, stable readout is often easier to range with than a precise, jittery
  one.

**Open question.** Is RSSI even the right ranging signal, or should the hunter's
**audio volume gradient** stay the primary cue with RSSI demoted to a coarse
"in range / marginal / strong" 3-state indicator? Recommend trying the EMA +
re-fit first (small change) before redesigning the indicator.

---

## 3. Per-device RSSI offset

**Observed.** As expected, RSSI varies quite a bit between units — different
boards and antennas. (We already have this documented: the RAK4631 reads
~41 dB lower than a Heltec V4 with equal SNR — the bar misleads, the radio is
fine.)

**Implication for §2.** Any perceptual re-fit must be **relative**, not a single
absolute dBm→px table baked for one board. Options:
- Per-board RSSI offset constant (calibrated once, stored in NVS alongside the
  `model` identity), normalizing the bar so "full" means the same closeness on
  every unit.
- Or sidestep absolute calibration entirely by making the bar **auto-ranging**:
  track the strongest RSSI seen this session as the top of scale. Simpler, no
  per-unit cal, and it naturally adapts to whatever board/antenna a hunter
  carries — at the cost of an absolute reading.

**Recommendation.** Auto-ranging EMA bar is the lowest-effort path that helps
both §2 and §3 at once.

---

## 4. Instructor ACK only received when close

**Observed.** Remote command of the fox via the instructor worked **every time**,
but the ACK was only received when the instructor was close to the fox. The
command link (instructor→fox) is fine; the **return** link (fox→instructor ACK)
is the weak leg.

**Root cause.** The ACK is transmitted at the **fox's current TX power**, which
in normal play is **Low** (§1). So the command gets through at the fox's good RX
sensitivity, but the ACK goes out underpowered and dies at distance. The
multi-ACK burst we added (`ACK_REPEATS=4 @ ACK_GAP_MS=50`, see
`main.cpp:1685`) fixed the *BLE-preemption* loss, but does nothing for a
*power*-limited ACK — four weak copies are still weak.

**Proposed fix (recommended): always ACK at MAX.** In the ACK path
(`instructor_control_rx` → the `ACK_REPEATS` loop), bracket the burst with a
temporary power bump:
1. Save current power (and FEM PA state).
2. `radio::set_tx_power(MAX)` + engage `radio::set_pa(true)` on FEM boards.
3. Send the ACK burst.
4. Restore the saved power/PA before `radio::start_receive()`.

The ACK is a tiny, infrequent packet, so the duty-cycle/§15.249 cost of sending
it hot is negligible, and the asymmetry is exactly right: command at play power,
acknowledgement at full power so the instructor always hears it.

**Alternative considered: power-matched ACK.** Have the instructor's control
packet carry a target power the fox must match on its ACK. More flexible (lets
the instructor demand a specific level) but more protocol surface (new field in
`ControlCmd`, version bump) for little practical gain. **Always-MAX is simpler
and almost certainly good enough** — go with that; revisit matched-power only if
we later want the ACK to probe link margin at a chosen level.

**Implementation notes.**
- `radio::set_tx_power(int dbm)` and `radio::set_pa(bool)` already exist
  (`radio.h:24,41`); this is a save/set/restore around the existing burst, no
  new protocol.
- On the V4.3 (KCT8103L) there is no CPS bypass, so "MAX" there is just the chip
  ceiling — fine.
- Make sure to restore power even on the early-return / dup-seq paths so we
  don't accidentally leave the fox keying its *message* at MAX afterward.

---

## 5. Signal loss at the edge of range → `????` on display

**Observed.** At the very edge of receiving range there was heavy signal loss,
producing lots of `????` on the hunter display.

**Assessment.** This is the **expected** behavior of the edge-event protocol's
loss handling: a dropped `EdgeEvent` now renders as `?` rather than a guessed
letter (`protocol.h:108`, the dur_prev "no longer heals" comment). At the edge
of range that's honest — better a visible `?` than a confidently wrong letter.
**Probably acceptable.**

**Optional improvements (only if we want to push usable range):**
- The "raw letters" idea in §7 directly attacks this: a retransmitted text
  packet is far more robust at the edge than a stream of timing edges where any
  single loss punches a hole.
- Narrower RX bandwidth buys sensitivity — we already moved the default to
  23.4 kHz with no measured loss penalty on the bench; confirm it's actually set
  in the field config.
- A coarse "link marginal" indicator (tie into §2's 3-state) would tell a hunter
  the `????` is range, not a fault, so they know to close distance.

**Recommendation.** Leave the `????` behavior as-is for live keying; treat §7 as
the real fix for the canned-message case.

---

## 6. Broadcast "alert" should be sticky + halt the fox

**Observed.** For the broadcast alert (`bcast`), the message should **stay on
screen until the device is reset or receives an `alert clear`**, rather than
auto-expiring. Additionally, a **fox** that receives an alert should **stop
transmitting** until reset, `alert clear`, or an explicit `start`.

**Current behavior.** The banner is deliberately transient: `set_banner()` arms
it for `BCAST_SHOW_MS` (15 s overlay) and `banner_active()` checks a timeout
(`main.cpp` ~L1694–1710). It is display-only; it has no effect on the fox's TX
loop. So today an alert flashes for 15 s and the fox keeps transmitting through
it.

**Proposed changes.**
- **Sticky alert mode.** Distinguish a *broadcast banner* (current transient
  behavior — fine for routine messages) from an **alert** that latches. When the
  alert flag is set, ignore the timeout and keep the banner up until an explicit
  clear. Concretely: a sentinel `g_banner_until` (e.g. `UINT32_MAX` / a separate
  `g_alert_latched` bool) that `banner_active()` treats as never-expiring.
- **`alert clear` command + over-the-air clear.** We already have `bcast clear`
  / empty-text dismissal locally (`set_banner` empties on `!text[0]`); add an
  `alert` verb (or reuse `bcast -a` semantics) that latches, and make the
  instructor's clear reach every station the same way the alert did.
- **Halt the fox on alert.** On receiving a latched alert, a node in fox mode
  should drop out of its TX loop (stop keying / stop the message cycle) and go
  quiet. Resume only on: device reset, `alert clear`, or an explicit `start`.
  This makes "alert" a real **all-stop** for the exercise (e.g. recall the
  hunters, safety stop), not just a visual.

**Design questions to settle.**
- Does the fox-halt also apply to instructor-relayed `start`/`stop`, i.e. should
  `relay <fox> start` override a latched alert? (Probably yes — explicit
  instructor command wins.)
- Should the latched alert survive a power-cycle (persist in NVS) or clear on
  reset? The note says "until device is reset," so **reset clears it** — do *not*
  persist. Keep it RAM-only.
- One alert at a time, or a stack? Recommend single latched alert; a new alert
  replaces the current banner text.

---

## 7. Open question — send raw letters instead of Morse edges (for canned messages)

**The idea.** For the **canned message** exercise (not live keying), transmit the
**plaintext letters** over the air and render Morse *on the receiving device*,
rather than streaming `EdgeEvent` key-up/down timings and decoding them on the
hunter. Pair this with retransmission to drive error rate toward zero.

**Why it helps.**
- **Robustness at the edge of range (§5).** Today every character is carried as
  a sequence of timing edges (`EdgeEvent`, 8 bytes each, `protocol.h:102`); a
  single lost edge becomes a `?`. A short text packet (e.g. the whole clue in
  one frame) either arrives or doesn't, and **retransmission** of that small
  frame makes eventual delivery near-certain. Far more bits-per-character
  efficient and far more forgiving of loss.
- **Consistent rendering.** The hunter generates clean Morse locally from the
  text at the timing it chooses (it already retunes its decoder from the IDENT
  packet's wpm/char_wpm — `protocol.h`), so the audio is crisp regardless of RF
  conditions, and there's no decode ambiguity.

**Why it does NOT replace live keying.**
- Live keying's whole point is real-time, human-timed edges — there's no text to
  send ahead of time, and the edge stream *is* the signal. The note correctly
  scopes this to canned messages only. So this becomes a **second message mode**,
  not a replacement: keep `EdgeEvent` for live key, add a text-frame mode for
  canned clues.

**Sketch.**
- New magic (e.g. `MAGIC_TEXT`) carrying `station_id`, `seq`, and a
  NUL-terminated clue string (the clue already fits — `FOX_MSG_MAX` is 96, and
  `CTRL_CMD_MAX` is 100, so the frame is small).
- Fox in canned mode transmits the text frame on a cycle and **repeats** it N
  times per cycle (like the ACK burst) for loss tolerance; hunter dedups by seq,
  renders Morse locally for audio + shows the decoded text directly (no `????`).
- Hunters that only understand edges ignore the new magic (same forward-compat
  pattern we use for IDENT/EDGE), so mixed fleets still work.

**Trade-offs / decisions.**
- **Realism.** Decoding *received Morse by ear* is part of the exercise's point.
  Sending text and re-rendering locally means every hunter hears identical,
  perfect Morse — arguably *less* authentic but *more* learnable. Decide whether
  the canned exercise is meant to train **copying real off-air Morse** (keep
  edges) or **practicing Morse reading in clean conditions** (use text). Could be
  a per-exercise switch.
- **Effort.** This is a meaningful new protocol + mode, not a tweak. Recommend it
  as a **follow-on**, after the §4 (always-MAX ACK) and §6 (sticky alert + fox
  halt) fixes, which are smaller and address confirmed field problems.

**Could the instructor and the message share one packet format?** Worth
exploring as part of this. If canned clues become text frames (§7), the wire
representation of a clue becomes "an ASCII string addressed to a station" — which
is *almost exactly* what the instructor control packet already is. The
`ControlCmd` (`protocol.h:203`) carries `src_id`, `target_id`, `seq`, and a
NUL-terminated ASCII line that the receiver runs through `handle_setup_command()`.
A text clue is the same shape: id + seq + ASCII payload. The two could converge
into **one framed-text packet with a `delivery`/`kind` field** distinguishing,
e.g.:
- `CMD` — run the payload as a console command (today's instructor behavior),
- `MSG` — treat the payload as a Morse clue to render/transmit,
- (room for more: `ALERT` text for §6, `IDENT`, etc.)

**Upside.** One encode/decode path, one dedup-by-seq mechanism, one
retransmit-burst helper, one ACK path — instead of parallel implementations for
control vs. message. The multi-copy burst + seq-dedup logic we built for ACKs
(§4) and would build for text retransmit (§7) is *identical*, so unifying avoids
writing it twice. Fewer magics, less protocol surface to keep forward-compatible.

**Caution.** Don't over-merge. A `delivery` discriminator is clean; folding
genuinely different semantics behind one magic can also make the parser a
grab-bag. Keep the live-keying `EdgeEvent` stream **separate** (it's a real-time
timing stream, not a framed ASCII payload — different problem). The merge worth
doing is **ControlCmd ⊕ text-MSG** (both are "addressed ASCII payload + seq"),
not all four packet types into one. Recommend prototyping the unified
addressed-text frame when we build §7, and retrofitting the instructor path onto
it if it lands cleanly.

---

## 8. Text mode broke field use (sync / RSSI / per-element)

`msgmode text` (§7) fixed received-message *clarity* — the clue arrives as
ground-truth ASCII, no `????` holes — but in the field it surfaced three new
problems, all from the same root cause: **the audio is generated locally and
independently on each hunter, and the air goes idle ~12 s (`REPEAT_PAUSE`)
between bursts.** That is exactly what fixed clarity, but it discarded the
shared, continuously-present signal edge keying gave for free.

1. **Hunters are unsynchronized.** Each renders the clue from a `morse::Player`
   started off its *own* `millis()` whenever its copy of the burst arrived, so
   several hunters in earshot beep at different times and the Morse smears into
   an unreadable cacophony.
2. **RSSI tracking has nothing to track.** Hunters DF the fox by RSSI, but text
   mode puts ~150 ms of traffic on the air then idles ~12 s. The bar is dead
   almost the whole time. DF needs a signal at **≤250 ms** cadence.
3. **Display reveals per character, not per element.** `reveal_to()` pushes a
   whole character's dit/dah pattern at once, so a learner can't watch each
   `.`/`-` light up in step with its beep.

**Design + plan:** `docs/plan-text-sync-beacon.md`. A periodic **sync/DF beacon**
(`MAGIC_SYNC`) from the fox gives every hunter a common time base to slave its
local render to (problem 1) *and* a fixed-cadence carrier to DF (problem 2);
per-element reveal (problem 3) is an orthogonal local display fix. The `TextMsg`
frame is unchanged — still the robust, no-`?` clue delivery and clean local
audio. Beacons are advisory: present, they lock all hunters onto the fox's clock;
absent, each hunter degrades to today's free-running-but-correct local render.
Includes mid-message join (a late station gets text from the 2 s `TextMsg`
re-send, then seeks to the live `pos_ms`).

**Implementation status: COMPLETE & HW-validated (branch `feat/text-sync-beacon`).**
- S1 ✅ `morse::Player::position_ms()`/`resync()` seek API (commit d1f01ae).
- S2 ✅ `proto::Sync`/`MAGIC_SYNC`/`POS_IDLE` + encode/decode (commit 678e570).
- S3 ✅ fox master render clock + `tx_sync()` beacons + 2 s clue re-send (faed020).
  Note: S3 had a stale-`now` underflow that left the clock stuck (every beacon
  `POS_IDLE`); fixed in 66fe8b8 (drive the fox render off `millis()`).
- S4 ✅ hunter `decode_sync` slave/seek/mid-join (aa70561). Validated 11/11 via
  `scripts/sync_beacon_test.py`: beacon ~212 ms median across render+pause,
  hunters slave the same clue, mid-join seeks to the live position.
- S5 ✅ per-element `reveal_to()` (f84194b) — one `.`/`-` per element as it sounds;
  `scripts/sync_reveal_test.py`.
- S5-fix ✅ display-freeze under `resync()` seeks (74ce5db): the reveal now slaves
  to `morse::Player::elems_elapsed()` (position-derived, seek-safe) instead of a
  free-running counter that could overshoot the total and strand the cursor.
  `scripts/morse_elems_test.sh`.

Manual-only checks remaining: audible 2-hunter unison by ear, antenna-pull
free-run/recover (both verified at the bench).

## Priority for next iteration

1. **§4 — Always-ACK-at-MAX.** Small, localized, fixes a confirmed every-time
   problem. Save/set/restore power around the existing ACK burst.
2. **§6 — Sticky alert + fox halt on alert.** Confirmed requirement; moderate
   change to banner lifetime + fox TX gating.
3. **§2/§3 — EMA-smoothed, auto-ranging RSSI bar.** Small change, big usability
   win for ranging; sidesteps per-device offset.
4. **§7 — Text-frame canned-message mode (with retransmit).** Larger follow-on;
   addresses §5 edge-of-range errors for canned clues. Keep edge mode for live
   key.

5. **§8 — Text-mode sync/DF beacon + per-element reveal.** In progress on branch
   `feat/text-sync-beacon` (S1–S3 landed; S4 hunter slaving next). Fixes the
   sync/RSSI/per-element regressions text mode introduced. See
   `docs/plan-text-sync-beacon.md`.

§1 and §5 are largely "working as intended" / operating-procedure notes.
