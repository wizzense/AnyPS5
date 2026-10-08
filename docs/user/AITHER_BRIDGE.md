# Aither bridge

An opt-in local socket that lets an external program watch frames and drive the pad. With it,
an agent or a test harness can play a converted title. It is off unless `APS5_AGENT_BRIDGE` is set.

```sh
APS5_AGENT_BRIDGE=47500 ./app.elf
```

The bridge listens on `127.0.0.1` only and accepts one client at a time. Port `0` picks a free port.
A value that is not a port number terminates the process, as any unsupported state does.

## Title to client

Newline-delimited JSON.

| Line | When |
|---|---|
| `{"t":"hello","proto":1}` | on connect |
| `{"t":"frame","n":<flip count>,"w":<width>,"h":<height>,"buf":<buffer index>,"ts_us":<steady clock µs>}` | after every completed flip |
| `{"t":"error","msg":"bad command"}` | a client line did not parse; the pad state is unchanged |

Sending is bounded by a 50 ms timeout. A client that stops reading is disconnected, and the title is never held up.

## Client to title

Plain text, one command per line.

| Command | Effect |
|---|---|
| `pad <buttons> <lx> <ly> <rx> <ry> <l2> <r2>` | hold this pad state until replaced. `buttons` uses the `Pad::PadButton` bits; sticks and triggers are 0–255, with sticks centred at 128 |
| `release` | stop overriding |

The override is merged into the local pad state: buttons are OR'd, the stick values replace the local
sticks, and the larger of each trigger value wins. When the client disconnects, the override is released.

Test: `aither_bridge_tests` (`ctest -R aither_bridge`).
