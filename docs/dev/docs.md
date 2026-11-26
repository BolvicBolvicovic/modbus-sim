# Documentation site

The site in `build/docs/` is generated from Doxygen's XML by `tools/docs/build.py` (Python 3
standard library only, deterministic output). Doxygen reads the Markdown pages and the C headers;
the generator renders them as one site with its own theme.

```bash
make docs                                  # Doxygen (warnings are errors), brief check, site
python3 -m http.server -d build/docs       # preview at http://localhost:8000
make docs-fixtures                         # reference-template fixtures into build/docs-fixtures
```

`build/docs/` holds the site (`index.html`, `user/`, `dev/`, `api/`, `assets/`), `build/docs/xml/`
(the generator's input) and `build/docs/internals/`, Doxygen's own HTML with the source and struct
browser for contributors. Pages that are not in the manifest, a link that does not resolve, an
unknown reference section or a reference entry out of order fail the build.

## The manifest

`docs/site.json` is the only place that decides what is on the site and in which order:

```json
{
  "home": "README.md",
  "home_nav": "Overview",
  "sections": [
    {"title": "Guide", "items": ["docs/user/get-started.md"]},
    {"title": "Reference", "items": [
      "docs/user/config/format.md",
      {"index": "docs/user/config/server/index.md", "title": "Server",
       "groups": [{"title": "Server", "entries": ["docs/user/config/server/mode.md"]}]},
      {"api": "include/mogbus.h", "title": "C API"}
    ]},
    {"title": "Contributing", "items": ["CONTRIBUTING.md"]}
  ]
}
```

- A string is a guide page (path relative to the repository root); `{"page": path, "title": ...}`
  overrides the title.
- `{"index": path, "entries": [...]}` (or `"groups": [{"title", "entries"}]`) declares a reference
  index and its entry pages. The entries become children of the index in the navigation.
- `{"api": "include/mogbus.h"}` generates the C API pages (see below).
- Every page Doxygen sees (`INPUT` in the `Doxyfile`) must be listed, otherwise the build fails
  naming the file. A new page therefore needs a manifest line.
- URLs follow the path: `docs/user/x.md` becomes `user/x.html`, `README.md` is `index.html`, other
  root files are lowercase (`contributing.html`). `base` (default `docs`) is the prefix stripped.

## Page conventions

Write plain Markdown: one `#` title, `##` and `###` sections, short pages. Rules the generator
relies on:

- Links between pages are relative `.md` links, optionally with `#heading`. Anchors are the
  GitHub-style slug of the heading; duplicates on a page get `-2`, `-3`. Links to files that are
  not pages use absolute GitHub URLs.
- Code fences carry a language (`scheme`, `bash`, `c`, `python`, `json`, `yaml`, `ini`,
  `powershell`, `text`). Code blocks that follow each other directly and have different languages
  become one tab group (Stripe-style language switcher, the choice is remembered); put a paragraph
  between blocks that are not alternatives. `text` blocks never join a group.
- Callouts are blockquotes that start with a bold keyword, which also reads well on GitHub:
  `> **Note:** ...`, `> **Warning:** ...`, `> **Tip:** ...`. In header comments `@note` and
  `@warning` give the same result.
- Titles may use backticks, as in a heading that names a config form.

## Reference entries and indexes

A reference **entry** is one config key, behaviour, fault kind, function code, and so on. It is a
page listed under an `index` in the manifest (that is how a page declares itself an entry; there is
no per-page marker). Layout:

```markdown
# `(ramp ...)`

One-sentence summary.

## Syntax

(code block with the syntax line)

## Default

`mode wrap`

## Valid in

- `(holding ...)`

## Behaviour
## Errors
## Examples
## See also
```

- The first paragraph is the summary: shown as the lead and in the index list and search results.
- The `##` sections are optional, but only these names, in this order: `Syntax`, `Default`,
  `Valid in`, `Behaviour`, `Errors`, `Examples`, `See also`.
- `Syntax` (its code block), `Default` and `Valid in` (a bullet list renders as chips) become one
  compact definition block at the top. `Examples` moves into the sticky right column; below
  1100 px it appears inline before `See also`. Other sections render in the reading column;
  `###` subsections are allowed inside them.
- An **index** page is a short Markdown page (title and intro). The generator appends a filter box
  and the list of its entries (title and summary, with the manifest's group headings). Typing narrows
  the list as you type.

`tools/docs/fixtures/` holds one index and three entries (`(server (mode ...))`, `(ramp ...)`,
`(fault (kind delay))`) that exercise the template. They are built by `make docs-fixtures` into
`build/docs-fixtures/` and are never part of the real site. Start a new entry by copying one.

## C API pages

`include/mogbus.h` is rendered as `api/index.html` (the file comment and a symbol table) and one page
per group. Groups come from the `/* ---- Name ---- */` marker comments in the header (Doxygen
does not keep them, so the generator reads the header and assigns each symbol by line number; text
in parentheses after the name is dropped from the title). Each function is a block with a stable
anchor equal to its name: signature in the right column, brief, details, parameter table and return
value from `@param` and `@return`. Macros are one table per group; enums, structs and opaque typedefs
get their own blocks. A new group needs only a new marker comment.

## Search and theme

The generator writes `assets/search-index.json` (and the same data as `search-index.js`, which the pages
load so the site also works when opened directly from disk): title, section, summary, `##` headings and anchors. The
header search box (`/` focuses it, arrows and Enter navigate) runs entirely in the browser.
The theme is `tools/docs/theme/style.css` and `site.js` (no frameworks, no CDN, system fonts; light
and dark with a toggle). Syntax highlighting is a small tokenizer in `site.js`.

`tools/docs/shoot.sh` takes screenshots with Playwright's Chromium in podman for visual checks, for
example `tools/docs/shoot.sh /tmp/shots build/docs index.html:1440x900:full`.

## Doxygen constraints

The XML of Doxygen 1.9.8 (the CI version) and newer must both be warning-free. Markdown titles keep
backticks as `<tt>` text and heading ids are renumbered across pages (`behaviour-2`), which is why the
generator derives its own anchors and only uses Doxygen's ids to resolve links. See
[Architecture](architecture.md) for the comment rules.
