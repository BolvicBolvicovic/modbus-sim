# Documentation site

The site in `build/docs/` is generated from Doxygen's XML by `tools/docs/build.py` (Python 3
standard library only, deterministic output). Doxygen reads the Markdown pages and the C headers;
the generator renders them as one site with its own theme.

```bash
make docs                                  # Doxygen (warnings are errors), brief check, site
python3 -m http.server -d build/docs       # preview at http://localhost:8000
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
- `{"api": "include/mogbus.h"}` generates the C API pages and `{"pyapi": "python/mogbus/core.py",
  "groups": [{"title", "items": [names]}]}` the Python API pages (see below).
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
  `powershell`, `text`). Only alternatives are grouped: code blocks that follow each other
  directly in different languages (`bash`, `python`, `scheme`, ...) become one tab group (Stripe-style
  language switcher, the choice is remembered). `json`, `text` and unlabelled blocks are output or
  data and never join a group, so a request followed by its response, or a command followed by its
  output, stays visible. In the examples column a `json`/`text` block directly after a `bash` block
  is labelled Response. Put a paragraph between blocks that are not alternatives.
- Callouts are blockquotes that start with a bold keyword, which also reads well on GitHub:
  `> **Note:** ...`, `> **Warning:** ...`, `> **Tip:** ...`. In header comments `@note` and
  `@warning` give the same result. `> **Summary:** ...` is the purple summary box of a tutorial
  step: put it right under the page title, as the page's first paragraph; it is also the page
  summary used by the search index and meta description.
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
  compact definition block at the top. `Examples` moves into the sticky right column (long lines wrap there); below
  1360 px it appears inline before `See also`. Other sections render in the reading column;
  `###` subsections are allowed inside them.
- An **error index** is a guide page declared as `{"page": "docs/user/config/errors.md", "errors": true,
  "errors_from": ["docs/user/config/format.md"]}`. Its Markdown is a short intro; the generator appends one
  table per reference index under the page's directory (message, link to the entry's Errors section) built
  from the `## Errors` bullets of every entry, plus the guide pages listed in `errors_from`. The message of
  a bullet is its first code span (with any `` `A` / `B` `` alternatives); write
  ``- `message`: what to do``. A bullet that does not start with a code span fails the build.
- An **index** page is a short Markdown page (title and intro). The generator appends a filter box
  and the list of its entries (title and summary, with the manifest's group headings). Typing narrows
  the list as you type.

Start a new entry by copying a real one, for example
[`docs/user/config/server/char-timing.md`](https://github.com/BolvicBolvicovic/mogbus/blob/main/docs/user/config/server/char-timing.md).

## C API pages

`include/mogbus.h` is rendered as `api/index.html` (the file comment and a symbol table) and one page
per group. Groups come from the `/* ---- Name ---- */` marker comments in the header (Doxygen
does not keep them, so the generator reads the header and assigns each symbol by line number; text
in parentheses after the name is dropped from the title). Each function is a block with a stable
anchor equal to its name: signature in the right column, brief, details, parameter table and return
value from `@param` and `@return`. Macros are one table per group; enums, structs and opaque typedefs
get their own blocks. A new group needs only a new marker comment.

## Python API pages

`python/mogbus/core.py` is rendered as `pyapi/index.html` (the module docstring, which is a Doxygen
`@file` comment, and a table per group) and one page per `groups` entry of the manifest. Doxygen reads
the docstrings (`PYTHON_DOCSTRING = NO`, so `@brief`, `@param`, `@return`, `@throws` and Markdown
work; `_lib.py` and `__init__.py` are excluded and `_private` names are skipped). Names, signatures,
dataclass fields and enum values come from the source through `ast`, because Doxygen parses type
annotations badly; descriptions come from Doxygen's XML. Rules, each a build error when broken:

- every public class, function and method has a `@brief`; every parameter of the signature has a
  `@param` and nothing else is documented; enum members and class attributes have a `##` comment
  line above them;
- dataclass and `NamedTuple` fields are documented with `@param` in the class docstring (Doxygen
  does not see fields without a default);
- every public name is listed in exactly one manifest group.

A class page block lists its members in a table and each member follows as its own block with the
anchor `Class.member`. Markdown pages link to Python symbols with `\ref mogbus::core::Device::add_point
"Device.add_point()"` and to the overview with `\ref core.py "Python API"`; relative `.md` links
inside a docstring are relative to the `.py` file.

## Search and theme

The generator writes `assets/search-index.json` (and the same data as `search-index.js`, which the pages
load so the site also works when opened directly from disk): title, section, summary, `##` headings and anchors. The
header search box (`/` focuses it, arrows and Enter navigate) runs entirely in the browser.
The theme is `tools/docs/theme/style.css` and `site.js` (no frameworks, no CDN, system fonts; light
and dark with a toggle). Syntax highlighting is a small tokenizer in `site.js`.

## Doxygen constraints

The XML of Doxygen 1.9.8 (the CI version) and newer must both be warning-free. Markdown titles keep
backticks as `<tt>` text and heading ids are renumbered across pages (`behaviour-2`), which is why the
generator derives its own anchors and only uses Doxygen's ids to resolve links. See
[Architecture](architecture.md) for the comment rules.
