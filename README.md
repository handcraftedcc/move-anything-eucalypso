# Eucalypso Module for Move Everything

Euclidean MIDI sequencer module for Ableton Move, built for Move Everything.

## Features

Eucalypso is a chainable MIDI FX module (`midi_fx`) for Move Everything. It generates up to 4 parallel Euclidean lanes with deterministic timing and seeded per-lane note variation.

- Internal clock or external MIDI clock sync
- Hold/latch play modes with restart/continuous retrigger behavior
- Rate, swing, voice limit, and global velocity/gate controls
- 4 independent Euclidean lanes (steps, pulses, rotation, drop + drop seed)
- Per-lane register note index and octave offsets
- Per-lane note randomization and octave randomization with dedicated seeds
- Global velocity and gate randomization with shared seed/cycle controls
- Note register modes for held notes or scale-derived notes
- Held-note ordering (up, down, played, rand with seed)
- Scale selection, scale range, root, and base octave controls

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: `http://move.local/development/ssh`

## Installation

### Via Module Store

If Eucalypso is published in Module Store:

1. Launch Move Everything on your Move
2. Select **Module Store**
3. Navigate to **MIDI FX** -> **Eucalypso**
4. Select **Install**

### Manual Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Usage

Eucalypso is a MIDI FX module:

1. Insert **Eucalypso** in a chain's MIDI FX slot
2. Play and hold notes/chords into the chain
3. Route output to a synth/sampler module
4. Configure timing in **Global**, note pool behavior in **Note Register**, and pattern behavior in each **Lane**

## Note Emission Behavior

- Timing always advances from transport/clock; lane rhythm phase does not depend on incoming note timing.
- Note output is gated by the active note state:
- `hold` mode: output only while notes are physically held.
- `latch` mode: output from the latched note set after keys are released, until that set is replaced.
- `register_mode=held`: lane note indices select from the active held/latched note register.
- `register_mode=scale`: lane note indices select from the configured scale register, but output is still gated by active held/latched state.
- If a lane note index is out of register range, `missing_note_policy` decides behavior (`skip` default, `fold`, `wrap`, or seeded `random`).

## Parameters

In Shadow UI, parameters are organized into sections.

### Global

| Parameter | What it does |
|---------|--------|
| `play_mode` (`Play`) | Selects `hold` or `latch`. |
| `rate` (`Rate`) | Step division from `1/32` up to `1`. |
| `retrigger_mode` (`Retrig`) | `restart` or `cont` sequence behavior on new trigger. |
| `sync` (`Sync`) | Selects `internal` or MIDI `clock`. |
| `bpm` (`BPM`) | Internal tempo (`40-240`) when `sync=internal`. |
| `swing` (`Swing`) | Swing amount (`0-100`). |
| `max_voices` (`Voices`) | Limits simultaneous output voices (`1-64`). |
| `global_velocity` (`Vel`) | Global base velocity (`1-127`). |
| `global_v_rnd` (`Vel Rnd`) | Global velocity random amount (`0-127`). |
| `global_gate` (`Gate`) | Global base gate length (`1-1600`). |
| `global_g_rnd` (`Gate Rnd`) | Global gate random amount (`0-1600`). |
| `global_rnd_seed` (`Rnd Seed`) | Shared seed base for global random engines. |
| `rand_cycle` (`Rand Cyc`) | Loop length for deterministic random cycles (`1-128`). |

### Note Register

| Parameter | What it does |
|---------|--------|
| `register_mode` (`Reg Mode`) | Builds note register from `held` notes or selected `scale`. |
| `held_order` (`Note Ord`) | Note ordering: `up`, `down`, `played`, `rand`. |
| `held_order_seed` (`Rand Ord Seed`) | Seed for randomized note order mode. |
| `missing_note_policy` (`Miss Pol`) | Out-of-range lane note behavior: `skip`, `fold`, `wrap`, `random`. |
| `missing_note_seed` (`Miss Seed`) | Seed for randomized missing-note resolution. |
| `scale_mode` (`Scale`) | Chooses the active scale set for `scale` register mode. |
| `scale_rng` (`Scale Rng`) | Number of scale steps available in register (`1-24`). |
| `root_note` (`Root`) | Root note (`0-11`). |
| `octave` (`Oct`) | Global register octave offset (`-3` to `+3`). |

### Lanes (1-4)

Each lane has the same control set.

| Parameter | What it does |
|---------|--------|
| `laneX_enabled` (`On`) | Enables/disables lane output. |
| `laneX_steps` (`Steps`) | Lane pattern length (`1-128`). |
| `laneX_pulses` (`Pulse`) | Euclidean trigger count (`0-128`). |
| `laneX_rotation` (`Rot`) | Rotates lane pattern start. |
| `laneX_drop` (`Drop %`) | Drop chance amount (`0-100`). |
| `laneX_drop_seed` (`Drop Seed`) | Seed for lane drop decisions. |
| `laneX_note` (`Note`) | Register index used by the lane (`1-24`). |
| `laneX_n_rnd` (`Note Rnd`) | Chance/amount to swap with a random register note. |
| `laneX_n_seed` (`Note Seed`) | Seed for lane note randomization. |
| `laneX_octave` (`Oct`) | Lane octave offset (`-3` to `+3`). |
| `laneX_oct_rnd` (`Oct Rnd`) | Lane octave randomization amount (`0-100`). |
| `laneX_oct_seed` (`Oct Seed`) | Seed for lane octave randomization. |
| `laneX_oct_rng` (`Oct Rng`) | Octave randomization set (`+1`, `-1`, `+-1`, `+2`, `-2`, `+-2`). |
| `laneX_velocity` (`Vel`) | Lane velocity override (`0` uses global velocity). |
| `laneX_gate` (`Gate`) | Lane gate override (`0` uses global gate). |

## Troubleshooting

**No sequence output:**
- Verify Eucalypso is receiving MIDI notes
- Check lane `On`, `Steps`, and `Pulse` settings
- In `clock` sync mode, ensure external MIDI clock start/tick is present

**Unexpected note choices:**
- Check `register_mode` and `held_order`
- Verify `Scale`, `Scale Rng`, `Root`, and `Oct` values
- Review lane `Note`, `Note Rnd`, `Note Seed`, and `Oct` settings

**Timing behavior feels off:**
- Confirm `retrigger_mode` (`restart` vs `cont`)
- Confirm `sync` source (`internal` vs `clock`)
- Re-check `swing` and `rate`

## Building from Source

```bash
./scripts/build.sh
```

Build output:

- `dist/eucalypso/`
- `dist/eucalypso-module.tar.gz`

## Credits

- Move Everything framework and host APIs: Charles Vestal and contributors
- Eucalypso implementation: move-anything-eucalypso project contributors

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so validate functionality, security, and license compatibility before production use.
