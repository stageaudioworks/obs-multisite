# Contributing

If this is useful to your church, use it. If you improve it, we would be glad
to see the change come back. If it fails you in an interesting way, a good bug
report is a real contribution: much of what works well here was fixed because
someone took the time to paste a log.

---

## Sign your commits off

Every commit in a pull request needs a `Signed-off-by` line. Git writes it for
you:

```sh
git commit -s -m "your message"
```

which appends:

```
Signed-off-by: Your Name <you@example.com>
```

To sign off work you have already committed:

```sh
git commit --amend -s --no-edit          # the last commit
git rebase --signoff origin/main         # everything on your branch
```

The name and email must be real and must match the commit's author. Set them
once with `git config user.name` and `git config user.email` and you will not
think about it again.

A check on every pull request looks for the line, and says which commit is
missing it if one is.

## What signing off means

It means what the [DCO](DCO) says: you wrote the change, or you have the right
to pass on work that somebody else wrote under a compatible licence. You are
certifying provenance — that this code is yours to give — and nothing more.

**We deliberately do not ask for a Contributor Licence Agreement.** A CLA would
give Stage Audio Works the right to relicense your contribution, including into
a closed-source product. This project does not ask for that right, and the
reason is worth stating plainly rather than leaving you to infer it: the
commercial work built around this project is written separately and talks to it
over a network, so it never needs to take GPL code private. Nothing you
contribute here can be taken proprietary — not by us either. Your change stays
GPLv3, for the next church as much as for this one.

A DCO gives us what we actually need, which is confidence about where code came
from. Anything beyond that would be asking for a right we have no plan to use,
in exchange for nothing.

## Coding standards

The conventions this codebase follows are written down in
`docs/CODING-STANDARDS.md`, with the reason each one exists — they are observed
from the code rather than aspirational, and most are there because breaking one
produced a fault that reached an operator. `CONTEXT.md` defines the project's
vocabulary, which is worth two minutes before writing anything about time or
positions.

## Before you open a pull request

Read the [developer guide](docs/DEVELOPER.md) for building, the test suite, and
the conventions this codebase holds to. The parts that most often come up:

- **Run the tests.** `ctest` from your build directory. CI runs them on Linux,
  Windows and macOS, and the macOS runner exists to catch a specific class of
  regression, so a green run on one platform is not the whole answer.
- **A behaviour change wants a test.** Especially anything touching the storage
  protocol, the reliability path or the playout arithmetic — the failures there
  are quiet, and a quiet failure in a room full of people is the thing this
  project exists to avoid.
- **Say why in the commit message, not just what.** The diff already shows
  what changed. What it cannot show is the reasoning, the option you rejected,
  or the thing that bit you on the way. `git log` here is written that way on
  purpose, and it is most of the project's design record.
- **Four files are touched by nearly every change** — `BUGS.md`, `README.md`,
  `PROJECT-SCOPE.md` and `.github/RELEASE-NOTES.md`. Expect a rebase to reach
  them, and prefer appending your section to rewriting somebody else's
  paragraph to make room for it.

## Reporting a bug

A log is worth more than a description. The encoder and decoder docks both
show the last error, and `BUGS.md` records what has been found so far and how
each one was reproduced.

Useful to include: what the room was doing, what you expected, what happened
instead, the plugin version from the dock, the OBS version, and the platform.
If it involves a satellite, whether the link was healthy at the time — the dock
reports that too, along with which Cloudflare edge served the request, which
has explained more than one baffling latency.

## Licence

This project is GPLv3. By contributing under the DCO you are submitting your
work under that licence, and it stays under it. See [LICENSE](LICENSE) and
[COPYRIGHT](COPYRIGHT).
