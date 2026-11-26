# Fixture site

This site exists only to prove the reference template: an index page and three entry pages.
It is built by `make docs-fixtures` and never ships with the real documentation.

> **Note:** a callout written as a blockquote that starts with a bold keyword.

> **Warning:** the control API has no authentication.

## Guide-style content

Code blocks of different languages that follow each other become tabs:

```bash
./mogbus --check device.scm
```

```python
import mogbus
ctx = mogbus.Context()
```

```c
mogbus_t* ctx = mogbus_create();
```

| Column | Meaning |
|--------|---------|
| `a`    | first   |
| `b`    | second  |

### A subsection

See the [config keys](keys.md) or jump straight to [ramp](ramp.md#behaviour).
