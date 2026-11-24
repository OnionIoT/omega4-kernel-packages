# HPMCU startup ordering and panic investigation

On 24 November 2026, A29C showed two kernel panics on first boot after installing
the same diagnostic image (SHA256 fe9f0238b57be83e44f10ca1f265a23d76819e518f6e41b4cd69b5284b37df90).
Both occurred while omega4_hpmcu_loader was probing, after clocks were enabled
but before its firmware was loaded or the core reset. One crashed in the timer
interrupt; the second crashed in lru_add during request_firmware filesystem I/O.

The instruction bytes printed in both oopses were not kernel instructions from
the exact image. For the first, PC b0058d40 should contain Thumb halfwords
f103 02c0 (add.w), but the oops printed 5d80 5453. Nearby words differed too.
This establishes kernel text corruption; the timer/LRU functions are victims.

## Defect and correction

The driver enabled all HPMCU clocks in probe, then performed property parsing,
sysfs setup and request_firmware before asserting core reset. A cold boot with
HPMCU autoload disabled showed CRU reset register 0x20000a28=0 and code mapping
0x20150208=0: the core was not held in reset, and its address translation was
not yet the reserved 16 MiB firmware region. Clock-first startup can therefore
run the auxiliary CPU before its firmware and mapping are ready.

Hold core reset before enabling clocks, retaining it through firmware lookup,
copying and mapping setup. Fail probe when no usable reset control exists or
assertion fails. Existing startup releases reset only after setup. Also stop
the HPMCU in platform shutdown before system reboot. Increment package release
to 3. No diagnostic delays, fixed kernel addresses or module parameters belong
in the production patch.

## Evidence and limits

The startup race is a definite defect and a leading explanation for the memory
corruption. It is not an instruction-trace proof that the MCU performed the
specific corrupting writes. A diagnostic module with a 1-second clock-first
window did not corrupt the sampled words on either cold or warm manual loading
of the later audio image. Reinstalling the original diagnostic image did
reproduce a second panic. Tests of the corrected ordering are recorded below.

Workspace evidence: hardware-validation/2026-11-24-panic/uart.log, README.md,
loader-original.c, loader-diag.c, extracted diag-kernel.raw, and build logs.
The earlier original trace remains in 2026-11-24-audio-shutdown/uart.log.
One pre-fix warm reboot stalled in SPL kernel-hash checking; cold power restored
boot. This is recorded separately, not proven to share the same cause.

The corrected module (SHA256
98d74fef2ce5d33741014406c8479855c26747f1d29175b770fc94698e598b59)
passed a reboot as an overlay on the original diagnostic kernel. A fresh
sysupgrade combining that exact kernel payload with the corrected rootfs
(SHA256 7b123b2404e58de95f4c1803c647db494a75516189959bab81234ec84d9eb023)
passed first boot, without the old image's panic. The full current audio+HPMCU
candidate (SHA256 02d437e623ac84e70132a312e2c2391cf308a6ca02bddc983d10c89951cd2b57)
also passed first boot. Its log shows heartbeat increments, mailbox auto-ping
success, and HPMCU stopped by the shutdown handler on reboot.

Two further warm reboots of the full candidate passed with the shutdown stop
message and normal heartbeat/mailbox startup. A 96 MiB, eight-pattern
address-dependent memory smoke test passed, followed by stereo playback/capture
with normal runtime suspend and no idle warning. Network config/environment
hashes were unchanged. These bounded tests support the fix but do not rule out
all intermittent DDR faults or prove the precise bus writer behind the old
corruption. A29C is left on the full corrected candidate; no release was made.
