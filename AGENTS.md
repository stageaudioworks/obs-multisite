# obs-multisite

An OBS encoder/decoder plugin pair, a Raspberry Pi campus-player appliance, and
a simulcast relay, for broadcasting one service to several church locations.
GPL-3.0-or-later.

## Read these first

The important knowledge in this repository is in prose, not in the code, and
most of it is in two long files. Reading the relevant part of them is not
optional for anything touching playout, seeking, timing or storage.

| File | What it is the authority for |
| --- | --- |
| `CONTEXT.md` | The vocabulary. Short. Read it before writing about time or positions. |
| `PROJECT-SCOPE.md` | How each component is meant to work. Numbered sections; cite them. |
| `BUGS.md` | Every fault, its root cause, what was measured, and which attempts were reverted. |
| `docs/CODING-STANDARDS.md` | The conventions this code follows, and why each one exists. |
| `docs/adr/` | Decisions taken from now on. Settled ones are in the two files above. |

**`BUGS.md` before timing code.** Seeking has been broken five times in one
afternoon, the hold/resume path three times, and the same zero-sentinel trap six
times. The entry usually names the thing you are about to try.

## The components

- **`src/core/`** — the engine. No OBS, no Qt. Muxing, decoding, storage,
  playout timing. This is where the tests are.
- **`src/obs/`** — the plugin: one binary that is both encoder and decoder,
  which role being a setting. Qt docks live in `src/obs/ui/`.
- **`src/appliance/`** — the Pi campus player. Same core, its own video/audio
  output and a web page instead of OBS.
- **`relay/`** — the simulcast service. Reads from storage exactly as a campus
  does and pushes to YouTube or SRT. A peer of a campus, not upstream of one.

## Working here

- **Other agents share `main`.** Fetch and check for collisions before every
  push, every time.
- **Tests are the deliverable, not the evidence.** A fix lands with a test
  verified to fail against the old behaviour. See standards §5.
- **Measure before claiming.** "Faster" is a claim about a number; report the
  number. Where a fix is defensive rather than diagnosed, say so.
- Build and test instructions are in `docs/DEVELOPER.md`. macOS plugin builds
  need `scripts/setup-mac-build.sh`.

## Agent skills

### Issue tracker

Issues live in GitHub Issues for `stageaudioworks/obs-multisite`, via the `gh`
CLI. See `docs/agents/issue-tracker.md`.

Note that the project's real fault record is `BUGS.md`, not the tracker, which
has never held an issue. Reports arrive directly from the operator.

### Triage labels

The five canonical triage roles, each mapped to a label string of the same
name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root. See
`docs/agents/domain.md`.
