# USB host — Linux golden register reference (working)

Captured live from Linux with USB re-enabled (`dr_mode="host"`), mouse enumerated
through the SL2.1A hub (`258a:0029` FS mouse on `1-1.1`, LED on). This is the
reference the MCU bare-metal host must match.

## dmesg (working path)
```
usb 1-1: new high-speed USB device number 2 using xhci-hcd
hub 1-1:1.0: USB hub found / 4 ports detected     (SL2.1A, self-powered, bMaxPower=100mA, bmAttributes=0xe0)
usb 1-1.1: new full-speed USB device number 3     (SINOWEALTH mouse, FS behind HS hub -> needs TT)
```

## DWC3 globals (base 0xffb0c000)
| reg | addr | Linux | MCU (before) | note |
|-----|------|-------|--------------|------|
| GCTL        | c110 | 0x30C11004 | 0x30C11004 | match |
| GSTS        | c118 | 0x7E800001 | — | status |
| GUCTL1      | c11c | **0x9505018A** | 0x1006018A | **DIFF** (bit17 PARKMODE_DISABLE_HS: Linux=0, MCU=1; plus L1/L2 device bits) |
| GSNPSID     | c120 | 0x5533300A | 0x5533300A | rev 3.00a |
| GUCTL       | c12c | **0x02004010** | 0x02000010 | **DIFF** bit14 HSTINAUTORETRY: Linux=1, MCU=0 |
| GUCTL2      | c19c | 0x0000040D | — | |
| GUSB2PHYCFG | c200 | 0x00101408 | 0x00101408 | match (16-bit UTMI, susphy/enblslpm/freeclk cleared) |
| GUSB3PIPECTL| c2c0 | 0x010A0002 | ~ | no SS |
| GFLADJ      | c630 | 0x00000000 | 0x00000000 | match |
| GSBUSCFG0   | c100 | 0x00000001 | | |
| GSBUSCFG1   | c104 | 0x00000300 | | |

## xHCI (base 0xffb00000, op base 0xffb00020)
| reg | addr | Linux |
|-----|------|-------|
| CAPLENGTH/HCIVER | 00000 | 0x01100020 |
| HCSPARAMS1 | 00004 | 0x01000140 (MaxPorts=1, MaxSlots=64) |
| HCCPARAMS1 | 00010 | 0x0220FE64 |
| USBCMD     | 00020 | 0x00000005 (RS=1, INTE=1) |
| USBSTS     | 00024 | 0x00000000 (running) |
| CONFIG     | 00038 | 0x00000008 (MaxSlotsEn=8) |
| PORTSC1    | 00420 | 0x00000E03 (CCS,PED,PLS=U0,speed=HS) |

## PHY analog trims (base 0xff3e0000) — ALL MATCH MCU
| reg | Linux | note |
|-----|-------|------|
| 0x30 | 0x0F | pre-emphasis SOF/EOP/chirp |
| 0x40 | **0x49** | HS Tx pre-emphasis bits[5:3]=**1** (cpu_version=1) — confirms the pre-emphasis fix |
| 0x64 | 0x80 | RX squelch |
| 0x70 | 0xB4 | HS disconnect single-ended (bit2) |
| 0x100| 0x00 | diff receiver off |
| 0x11c| 0xB7 | 45ohm ODT |
| 0x124| 0x0C | HS eye |
| 0x1a4| 0x11 | squelch cal bypass |
| 0x1b4| 0x11 | squelch cal bypass |

## GRF USB (base 0xff000000)
| reg | addr | Linux | MCU (forced-host expt) | note |
|-----|------|-------|------------------------|------|
| iddig  | 0050 | **0x8C00** | 0x8A00 | **DIFF**: Linux iddig_en=0 (use real ID pin), output=1. MCU forced host (en=1,out=0). Linux does NOT force iddig. |
| clkout | 0058 | 0x0000 | 0x0000 | match (480M clkout on) |
| utmi   | 0060 | 0x0644 | — | RO status: avalid=1, bvalid=1, iddig=1 |
| int_en | 0100 | 0x0000 | | |

## Key deltas MCU must apply
1. **iddig**: do NOT force host — match Linux `0x8C00` (write `0x06000400`). Host role comes from GCTL.PRTCAPDIR=HOST, not the ID pin. (Forcing host was tested and did not help downstream anyway.)
2. **GUCTL |= HSTINAUTORETRY (bit14)** → `0x02004010`. Host auto-retry on IN transaction errors (relevant for the FS mouse via the hub TT).
3. **GUCTL1** = `0x9505018A` (don't set PARKMODE_DISABLE_HS bit17; keep the L1/L2 decouple + hw-L1-exit device bits Linux sets).
4. Everything else (GCTL, GUSB2PHYCFG, GFLADJ, PHY analog, clkout) already matches.

Open question the golden does NOT explain: under the MCU the SL2.1A reports PORT_POWER
but never a downstream CONNECTION (mouse LED off), while Linux powers the port (LED on)
and the FS mouse connects. Since PHY/DWC3 statics match, the remaining suspect is the
hub-class software path (SET_CONFIGURATION on the hub + SetPortFeature(PORT_POWER) +
pgood delay) or the host keeping the bus active. Needs per-step instrumentation on the
MCU hub path.
