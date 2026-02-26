# Eucalypso for Move Everything

Eucalypso is a chainable `midi_fx` module for Ableton Move using Move Everything.

This repository is currently in a transition phase from a Super Arp codebase to a Euclidean sequencer architecture:

- Build and install scripts target `eucalypso`
- `module.json` currently uses a reduced Global + 4 Lane UI profile for compatibility/stability testing
- DSP keeps stable clock/MIDI/latch/sync infrastructure while arp-specific progression/rhythm engines are removed

## Build

```bash
./scripts/build.sh
```

Outputs:

- `dist/eucalypso/`
- `dist/eucalypso-module.tar.gz`

## Install

```bash
./scripts/install.sh
```

Installs to:

- `/data/UserData/move-anything/modules/midi_fx/eucalypso/`

## Requirements Source

Module requirements are tracked in:

- `agentfiles/Requirements.txt`
