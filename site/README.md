# The website

<https://stageaudioworks.github.io/obs-multisite/>

Three hand-written pages — `index.html`, `install.html` and `docs.html` — plus
one stylesheet. No framework, no CDN, no Node toolchain. `build.py` copies them
into `_site/`, substituting a handful of placeholders, and
[`.github/workflows/site.yml`](../.github/workflows/site.yml) publishes that to
GitHub Pages.

## What belongs here, and what does not

The site is the **front door**. The repository is the **manual**. Keeping the
two apart is the point: a site that mirrors the README has to be updated twice
for every change and will be wrong half the time.

So the site carries the things a church deciding whether to try this needs —
what it does, what it costs, what it cannot do, and how to install it — and
then links out to the repository for everything else. The known gaps, the
roadmap, the storage protocol, the operator and developer guides and the
release notes stay where they are and are **linked, never copied**.

The one exception is `Status`, which repeats the soak-test figures and the
"has not yet carried a real service" caveat. That is deliberate: a visitor
must not be able to reach the download button without meeting it.

## What updates by itself

`build.py` fills these in from the GitHub releases API and `CMakeLists.txt`:

| Placeholder | Becomes |
|---|---|
| `{{TAG}}` | the newest release tag, pre-releases included |
| `{{VERSION}}` | the version in the top-level `project()` |
| `{{RELEASE_URL}}` | that release's page |
| `{{DL_WINDOWS}}` `{{DL_MACOS}}` `{{DL_LINUX}}` | direct asset links |
| `{{FN_WINDOWS}}` `{{FN_MACOS}}` `{{FN_LINUX}}` | those assets' filenames |
| `{{BUILD_DATE}}` | the day the site was built |

A placeholder with no value fails the build rather than shipping `{{TAG}}` to a
reader. A platform missing from a release degrades to the releases page rather
than to a link that 404s.

The workflow runs on a push that touches `site/**`, `CMakeLists.txt` or
`site.yml`, when the "obs plugin build" workflow completes on main, and on
manual dispatch — so cutting a release refreshes the version and the download
buttons once its build finishes. (The `release` trigger is deliberately unused;
the workflow comment says why.)

## Editing it

```sh
python3 site/build.py && python3 -m http.server 8765 --directory _site
```

Then open <http://localhost:8765>. `build.py` needs nothing but a Python 3.9+
interpreter, and works offline — without the API it falls back to the
`CMakeLists.txt` version and the releases page.

Prose here is written for a church tech lead, not a developer: say "service"
rather than "event", spell out what a thing is for before what it is called,
and keep the honesty about alpha status in plain sight.

## A custom domain, later

Put the hostname in `site/CNAME` — one line, no scheme — and `build.py` will
copy it into the output. Then point the DNS at GitHub and set the domain under
**Settings → Pages** in the repository.
