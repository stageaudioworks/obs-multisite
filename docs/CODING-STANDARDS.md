# Coding standards

Conventions this codebase already follows, written down so a review has
something to check against. These are **observed**, not aspirational: each one
is here because breaking it produced a bug that reached an operator, and the
examples are real files.

Domain vocabulary is in `CONTEXT.md`. Faults and their root causes are in
`BUGS.md`. Read `BUGS.md` before changing playout, seeking or timing code.

## 1. A comment says why, and cites the measurement

Describing what the line does is noise; the compiler already said it. A comment
earns its place by recording the reasoning that is not in the code — usually
what was tried before, what it cost, and what the number was.

> `playout_clock.h`: *"READ THIS BEFORE 'FIXING' THE ARITHMETIC."*
>
> `multisite_source.cpp`: *"Measured at 0.20 s lost after a 1.4 s hold, 1.09 s
> after 11 s and 3.90 s after 67 s."*

A comment that will stop the next person re-making a reverted change is worth
ten that restate the syntax.

**Keep comments true when the code moves.** Two of the worst faults this year
were comments that were correct when written and silently became lies: *"audio
and video share it exactly as they did before"* after the skip moved to the
producer, and *"the gap between two real times"* after positions became media
times. If you move code, re-read what it says about itself.

## 2. One authority per quantity

A value derived in two places will drift. This is the most expensive pattern in
this codebase's history: `plays_as_recording` had five answers, the cue
conversion had two, segment length had two fallbacks, and the timeline and cue
list disagreed about where a cue sat.

If two call sites need the same number, they call the same function. If a
decision depends on two facts, **one** function reads both — do not split it and
let the second pass overwrite the first (the second-bucket settings panel did
exactly that and opened half exposed).

## 3. Zero is a value; `-1` means absent

Sequence numbers, positions and media times start at 0. `if (!x)` is therefore
not a test for missing — it tests *missing or at the very beginning*, and the
beginning is usually the ordinary case. Use `>= 0`, `Number.isFinite`, or an
explicit sentinel. This has been hit six times.

## 4. Constants that must agree are tied together

Where one constant only makes sense relative to another, say so in the code so
they cannot drift apart:

> `static_assert(kMaxQueuedNs > kMaxDeliveryLeadNs * 2, ...)`

The queue cap was sized from the delivery gate by hand, came out one frame
short, and jammed playback for weeks while both numbers looked reasonable
separately.

## 5. Every bug fix lands with a test that would have caught it

Not a test that passes — a test **verified to fail against the old behaviour**.
`test_seek_skip` was checked against a shim of the shared-base logic and fails
five ways there. A test written after the fix, never run against the bug, proves
only that the code does what it currently does.

Tests name the fault they pin:

> `test_playout_timeline.cpp`: *"Every case here is a bug that shipped."*

## 6. Moving code moves it out of its tests

Logic that crosses a layer takes its coverage and its guarantees with it. The
sub-segment skip was tested inside `PlayoutTimeline`; moving it to the producer
silently dropped that coverage **and** the presentation-ordering guarantee it
relied on, in a commit that was about performance.

When you move something: name the invariant the old site provided, say whether
the new one provides it, and check `tests/` still reaches it.

## 7. Make it testable by keeping the arithmetic out of the frame

Logic that needs OBS, a browser or a Pi to run will not be tested. Pure
arithmetic in its own header gets a test and keeps it: `seek_skip.h`,
`caption_text.h`, `media.js`, `position_interp.h`. The player's timeline broke
completely because its arithmetic lived in a file that needed a browser.

## 8. Logs answer a question, once

A log line exists so a specific question can be answered from a file after the
fact — "did the skip land on both streams?", "how long was delivery stalled?".
Say the numbers, not the adjectives. Rate-limit anything that could fire per
frame, and say a thing once per broadcast rather than once per event loop.

## 9. Measure before claiming

"Faster" and "fixed" are claims about numbers. Take the reading before and
after, and report the reading. Where a fix is defensive rather than diagnosed,
say which — `BUGS.md` distinguishes them deliberately.

## 10. Prose is for the reader, not the record

The fault record in this project became unreadable by keeping everything: a bug
entry that carried every measurement, every reverted attempt and every
correction grew to 470 lines, and the measurement that mattered sat 400 lines
below the paragraph that told you not to act on it. `BUGS.md` was 2,424 lines,
over half of it shipped work nobody had an action for.

The rule, and it applies to any long-lived document:

- **A working entry is one screen.** Status, symptom, root cause, next step,
  files, and the thing not to try. If it does not fit, the entry is several
  bugs or the detail belongs elsewhere.
- **The archive holds the archaeology, one file per subject** — `docs/bugs/`
  for faults, `docs/scope/` for design rationale. Keep it verbatim. Nothing is
  deleted; it is moved out of the path of someone trying to act.
- **A design doc states the specification, not its history.** What a thing does,
  what must be built, and the constraints; the "we considered X and rejected
  it", the measurements and the reverted attempts go to `docs/scope/`.
  `PROJECT-SCOPE.md` is cited as the authority, so a reader must be able to read
  a numbered section and know what to build without following a link.
- **Say the trap in the short form.** "Do not re-apply option (a)" belongs in
  the entry, not in a 470-line file the reader will not reach. The archive
  explains why; the entry says what.
- **Resolved work leaves `Open`.** An `Open` section that is mostly closed
  makes the project look worse than it is and hides the entries that are real.
- **Commit messages already hold the reasoning** for shipped work (see §11);
  do not restate it in an index. One line, and a pointer.

This mirrors §1: a comment earns its place by recording what is *not* in the
code. Prose earns its place the same way — by being where the reader needs it.

## 11. Housekeeping

- Git commits explain the reasoning, not the diff. `git log` here reads as a
  design record and is expected to.
- Other agents share `main`. **Fetch and check for collisions before every
  push.**
- Sign-off and the PR process are in `CONTRIBUTING.md`.
