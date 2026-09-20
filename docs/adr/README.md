# Architecture decision records

**Decisions already settled are not in here.** They are in `PROJECT-SCOPE.md`
(how a thing is meant to work) and `BUGS.md` (what went wrong and why the fix is
shaped the way it is). Those two files are the authority for everything decided
up to now, and they are 4,500 lines between them.

Retro-writing them as ADRs was considered and rejected. One quantity recorded in
two places drifts — that is the single most expensive pattern in this project's
history, and it is no less true of prose than of code. A decision restated here
would become a second answer that slowly disagrees with the first.

So: **ADRs start from here.** A new decision, taken from this point, that is
genuinely architectural — it constrains what can be built later, or it will look
arbitrary to someone who was not in the conversation — gets a file. Everything
else stays where it already lives.

## When to write one

Write an ADR when the decision:

- rules out an option someone will reasonably propose again, or
- has a cost that is worth accepting once and not re-litigating, or
- would otherwise only be explained in a commit message nobody will find.

Do not write one for a bug fix. That is a `BUGS.md` entry, which already records
root cause, measurement and the attempts that were reverted.

## Format

`NNNN-short-title.md`, numbered in order. Use `template.md`.

Keep them short. The reasoning belongs here; the specification belongs in
`PROJECT-SCOPE.md`, and this should link to it rather than duplicate it.

## Superseding

An ADR is never edited to change its decision — it is superseded. Mark the old
one `Superseded by ADR-NNNN` and leave the reasoning intact, because why a thing
was once right is usually the most useful part.
