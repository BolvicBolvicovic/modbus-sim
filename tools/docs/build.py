#!/usr/bin/env python3
"""Build the mogbus documentation site from Doxygen's XML.

Input:  build/docs/xml (Doxygen XML), docs/site.json (navigation manifest), include/mogbus.h
Output: build/docs/*.html plus assets/ (stylesheet, script, search index, images)

Standard library only and deterministic: no timestamps, everything sorted by the manifest
or by source order. Any broken link, page missing from the manifest or malformed reference
entry fails the build (exit status 1). See docs/dev/docs.md.
"""
import argparse
import ast
import copy
import html
import json
import posixpath
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
THEME = Path(__file__).resolve().parent / 'theme'

# Fixed section order of a reference entry page (docs/dev/docs.md).
ENTRY_SECTIONS = ['Syntax', 'Default', 'Valid in', 'Behaviour', 'Errors', 'Examples', 'See also']

LANG_ALIASES = {
    'sh': 'bash', 'shell': 'bash', 'console': 'bash', 'zsh': 'bash',
    'py': 'python', 'scm': 'scheme', 'h': 'c', 'cpp': 'c', 'yml': 'yaml',
    'ps1': 'powershell', 'txt': 'text',
}
LANG_NAMES = {
    'bash': 'Bash', 'c': 'C', 'python': 'Python', 'scheme': 'Scheme', 'json': 'JSON',
    'yaml': 'YAML', 'ini': 'INI', 'powershell': 'PowerShell', 'text': 'Text', '': 'Text',
}
SPECIAL = {
    'ndash': '–', 'mdash': '—', 'nonbreakablespace': ' ', 'copy': '©',
    'registered': '®', 'trademark': '™', 'lsquo': '‘', 'rsquo': '’',
    'ldquo': '“', 'rdquo': '”', 'hellip': '…', 'rarr': '→',
    'larr': '←', 'times': '×', 'deg': '°', 'plusmn': '±',
    'zwj': '\u200d', 'zwnj': '\u200c', 'le': '≤', 'ge': '≥', 'ne': '≠', 'bull': '•', 'sp': ' ',
}
CALLOUTS = {'note': 'note', 'warning': 'warning', 'tip': 'tip', 'important': 'warning',
            'caution': 'warning', 'attention': 'warning'}
BLOCK_TAGS = {'programlisting', 'table', 'itemizedlist', 'orderedlist', 'simplesect',
              'parameterlist', 'blockquote', 'para', 'verbatim', 'preformatted', 'hruler',
              'variablelist', 'sect1', 'sect2', 'sect3', 'sect4', 'title'}


def clean_title(t):
    """Doxygen leaves backticks of a Markdown title as <tt> text."""
    return re.sub(r'</?tt>', '', t)


def title_html(t):
    return re.sub(r'&lt;tt&gt;(.*?)&lt;/tt&gt;', r'<code>\1</code>', esc(t))


def esc(s):
    return html.escape(s, quote=True)


def slugify(s):
    s = re.sub(r'[^a-z0-9]+', '-', s.lower()).strip('-')
    return s or 'x'


def plain(e):
    """Plain text of an element (special-character elements resolved)."""
    out = [e.text or '']
    for c in e:
        if c.tag in SPECIAL:
            out.append(SPECIAL[c.tag])
        elif c.tag == 'linebreak':
            out.append(' ')
        else:
            out.append(plain(c))
        out.append(c.tail or '')
    return ''.join(out)


def norm_lang(name):
    lang = (name or '').lstrip('.').lower()
    return LANG_ALIASES.get(lang, lang)


class Fail(Exception):
    pass


class Page:
    """One generated HTML page."""

    def __init__(self, **kw):
        self.id = None          # Doxygen compound id (pages from Markdown)
        self.src = ''           # source file, for messages
        self.title = ''
        self.title_html = ''
        self.section = ''       # nav section title
        self.kind = 'guide'     # home | guide | entry | index | api
        self.url = ''
        self.summary = ''
        self.headings = []      # (level, id, text)
        self.anchors = set()
        self.body = ''
        self.parent = None      # index page for entries
        self.api = None         # API reference pages: the manifest entry (site.apis) they belong to
        self.errors_from = None  # error index page: guide pages that also contribute rows
        self.__dict__.update(kw)


class Site:
    def __init__(self, args):
        self.args = args
        self.xml = Path(args.xml)
        self.out = Path(args.out)
        self.manifest = json.loads(Path(args.manifest).read_text())
        self.errors = []
        self.compounds = {}      # refid -> kind (from index.xml)
        self.member_of = {}      # member refid -> compound refid
        self.pages = []          # in navigation order
        self.by_src = {}         # source path -> Page
        self.by_id = {}          # Doxygen compound id -> Page
        self.symbols = {}        # Doxygen refid -> url (API items)
        self.cur = None          # page being rendered (for error messages)
        self.nav = []            # [(section title, [node])]; node = dict
        self.apis = []           # API reference entries of the manifest ({api|pyapi} items)

    # ---- errors -----------------------------------------------------------------------------
    def error(self, msg):
        where = self.cur.src if self.cur is not None else ''
        self.errors.append('%s: %s' % (where, msg) if where else msg)

    # ---- loading ----------------------------------------------------------------------------
    def load_index(self):
        root = ET.parse(self.xml / 'index.xml').getroot()
        for c in root.findall('compound'):
            self.compounds[c.get('refid')] = c.get('kind')
            for m in c.findall('member'):
                self.member_of[m.get('refid')] = c.get('refid')

    def load_pages(self):
        found = {}
        for f in sorted(self.xml.glob('*.xml')):
            if f.name == 'index.xml':
                continue
            root = ET.parse(f).getroot()
            for cd in root.findall('compounddef'):
                if cd.get('kind') == 'page':
                    loc = cd.find('location')
                    src = loc.get('file') if loc is not None else cd.get('id')
                    found[src] = cd
        return found

    # ---- manifest ---------------------------------------------------------------------------
    def url_for(self, src):
        base = self.manifest.get('base', 'docs').rstrip('/') + '/'
        p = src[len(base):] if src.startswith(base) else src.lower()
        return re.sub(r'\.md$', '.html', p)

    def build_nav(self, found):
        m = self.manifest
        used = set()

        def make(src, kind, section, title=None, parent=None):
            if src not in found:
                self.error('manifest lists %s but Doxygen produced no page for it (is the file '
                           'under INPUT in the Doxyfile?)' % src)
                return None
            if src in used:
                self.error('%s is listed twice in the manifest' % src)
                return None
            used.add(src)
            cd = found[src]
            p = Page(id=cd.get('id'), src=src, kind=kind, section=section, parent=parent)
            p.cd = cd
            p.title = title or clean_title(plain(cd.find('title')).strip()) or Path(src).stem
            p.title_html = ''
            p.url = 'index.html' if kind == 'home' else self.url_for(src)
            self.pages.append(p)
            self.by_src[src] = p
            self.by_id[p.id] = p
            return p

        home = make(m['home'], 'home', 'Home')
        if home is None:
            return
        home.nav_title = m.get('home_nav', '')
        self.nav.append(('', [{'page': home}]))
        for sec in m['sections']:
            nodes = []
            for item in sec['items']:
                if isinstance(item, str):
                    p = make(item, 'guide', sec['title'])
                    if p:
                        nodes.append({'page': p})
                elif 'index' in item:
                    idx = make(item['index'], 'index', sec['title'], item.get('title'))
                    if idx is None:
                        continue
                    idx.groups = []
                    entries = []
                    groups = item.get('groups') or [{'title': '', 'entries': item.get('entries', [])}]
                    for g in groups:
                        lst = []
                        for src in g['entries']:
                            e = make(src, 'entry', idx.title, parent=idx)
                            if e:
                                lst.append(e)
                                entries.append(e)
                        idx.groups.append((g['title'], lst))
                    nodes.append({'page': idx, 'children': entries})
                elif 'api' in item or 'pyapi' in item:
                    py = 'pyapi' in item
                    self.apis.append({'file': item['pyapi' if py else 'api'], 'py': py,
                                      'title': item.get('title', 'Python API' if py else 'C API'),
                                      'section': sec['title'], 'groups': item.get('groups', []),
                                      'idx': len(self.apis)})
                    nodes.append({'api': len(self.apis) - 1})
                elif 'page' in item:
                    p = make(item['page'], 'guide', sec['title'], item.get('title'))
                    if p and item.get('errors'):
                        p.errors_from = item.get('errors_from', [])
                    if p:
                        nodes.append({'page': p})
                else:
                    self.error('unknown manifest item %r' % (item,))
            self.nav.append((sec['title'], nodes))
        missing = sorted(set(found) - used)
        if missing:
            self.error('pages not listed in the manifest (add them to %s): %s' %
                       (self.args.manifest, ', '.join(missing)))

    # ---- link resolution ----------------------------------------------------------------------
    def resolve(self, refid):
        if refid in self.by_id:
            return self.by_id[refid].url
        for pid in sorted(self.by_id, key=len, reverse=True):
            if refid.startswith(pid + '_1'):
                p = self.by_id[pid]
                slug = refid[len(pid) + 2:]
                if slug not in p.anchor_map:
                    self.error('link to unknown anchor #%s on %s' % (slug, p.src))
                    return None
                return p.url + ('#' + p.anchor_map[slug] if p.anchor_map[slug] else '')
        if refid in self.symbols:
            return self.symbols[refid]
        comp = refid if refid in self.compounds else self.member_of.get(refid)
        if comp is None:
            self.error('unresolved reference %s' % refid)
            return None
        if self.args.no_internals:
            return None
        target = comp + '.html'
        if not (self.out / 'internals' / target).exists():
            self.error('internal reference %s: no %s in internals/' % (refid, target))
            return None
        frag = ''
        if refid != comp and refid.startswith(comp + '_1'):
            frag = '#' + refid[len(comp) + 2:]
        return 'internals/' + target + frag



class Render:
    """XML description nodes to HTML."""

    def __init__(self, site):
        self.s = site

    # ---- inline ---------------------------------------------------------------------------------
    def inline(self, e):
        out = [esc(e.text or '')]
        for c in e:
            out.append(self.node(c))
            out.append(esc(c.tail or ''))
        return ''.join(out)

    def node(self, e):
        t = e.tag
        if t in SPECIAL:
            return esc(SPECIAL[t])
        if t == 'computeroutput':
            return '<code>%s</code>' % self.inline(e)
        if t == 'bold':
            return '<strong>%s</strong>' % self.inline(e)
        if t == 'emphasis':
            return '<em>%s</em>' % self.inline(e)
        if t == 'linebreak':
            return '<br>'
        if t == 'anchor':
            return ''
        if t == 'strike':
            return '<s>%s</s>' % self.inline(e)
        if t == 'ulink':
            url = e.get('url', '')
            m = re.match(r'^([^#:]*\.md)(?:#(.*))?$', url)
            if m:
                # Doxygen 1.9.8 leaves links with a #fragment as plain links
                return self.md_link(e, m.group(1), m.group(2))
            if not re.match(r'^(https?://|mailto:)', url):
                self.s.error('link %r is not absolute (use an absolute URL or a relative .md link)' % url)
                return self.inline(e)
            return '<a href="%s" rel="noopener">%s</a>' % (esc(url), self.inline(e))
        if t == 'ref':
            url = self.s.resolve(e.get('refid'))
            inner = self.inline(e)
            if url is None:
                return inner
            return '<a href="%s">%s</a>' % (esc(self.s.cur_root + url), inner)
        if t == 'image':
            if e.get('type') != 'html':
                return ''
            name = e.get('name', '')
            return '<img class="doc-img" src="%sassets/%s" alt="">' % (self.s.cur_root, esc(name))
        if t == 'ref' or t in ('subscript', 'superscript'):
            return self.inline(e)
        if t in ('verbatim',):
            return '<code>%s</code>' % esc(plain(e))
        if t in ('heading',):
            return self.inline(e)
        self.s.error('unsupported inline element <%s>' % t)
        return self.inline(e)

    def md_link(self, e, path, frag):
        site = self.s
        src = posixpath.normpath(posixpath.join(posixpath.dirname(site.cur.src), path))
        tgt = site.by_src.get(src)
        if tgt is None:
            site.error('link to %s: no such page in the manifest' % src)
            return self.inline(e)
        url = tgt.url
        if frag:
            if frag in tgt.anchor_map:
                frag = tgt.anchor_map[frag]
            elif frag not in tgt.ids.values():
                site.error('link to unknown anchor #%s on %s' % (frag, src))
                return self.inline(e)
            if frag:
                url += '#' + frag
        return '<a href="%s">%s</a>' % (esc(site.cur_root + url), self.inline(e))

    # ---- blocks ---------------------------------------------------------------------------------
    def items(self, e):
        """Content of a container as a list of (kind, ...) items; code blocks stay separate so
        that adjacent ones can be grouped into tabs."""
        out = []
        buf = [esc(e.text or '')]

        def flush():
            s = ''.join(buf).strip()
            if s:
                out.append(('html', '<p>%s</p>' % s))
            buf.clear()

        for c in e:
            if c.tag in BLOCK_TAGS - {'para', 'title'}:
                flush()
                out.extend(self.block(c))
            elif c.tag == 'para':
                flush()
                out.extend(self.items(c))
            elif c.tag == 'title':
                pass
            else:
                buf.append(self.node(c))
            buf.append(esc(c.tail or '') if c.tag not in ('para',) else esc(c.tail or ''))
        flush()
        return out

    def code_text(self, e):
        lines = []
        for cl in e.findall('codeline'):
            lines.append(plain(cl))
        return '\n'.join(lines)

    def block(self, c):
        t = c.tag
        if t == 'programlisting':
            return [('code', norm_lang(c.get('filename')), self.code_text(c))]
        if t in ('verbatim', 'preformatted'):
            return [('code', '', plain(c).strip('\n'))]
        if t == 'table':
            return [('html', self.table(c))]
        if t in ('itemizedlist', 'orderedlist'):
            tag = 'ul' if t == 'itemizedlist' else 'ol'
            lis = []
            for li in c.findall('listitem'):
                lis.append('<li>%s</li>' % self.group(self.items(li), tight=True))
            return [('html', '<%s>%s</%s>' % (tag, ''.join(lis), tag))]
        if t == 'blockquote':
            return [('html', self.blockquote(c))]
        if t == 'hruler':
            return [('html', '<hr>')]
        if t == 'simplesect':
            return [('html', self.simplesect(c))]
        if t == 'parameterlist':
            return [('html', self.params(c))]
        if t == 'variablelist':
            return [('html', self.group(self.items(c)))]
        if t.startswith('sect'):
            return []   # handled by the page renderer
        self.s.error('unsupported block element <%s>' % t)
        return []

    def table(self, c):
        rows = []
        for r in c.findall('row'):
            cells = []
            for en in r.findall('entry'):
                tag = 'th' if en.get('thead') == 'yes' else 'td'
                al = en.get('align')
                attr = ' class="a-%s"' % al if al in ('center', 'right') else ''
                inner = self.group(self.items(en), tight=True)
                cells.append('<%s%s>%s</%s>' % (tag, attr, inner, tag))
            rows.append(cells)
        head = ''
        if rows and r_is_head(c.findall('row')[0]):
            head = '<thead><tr>%s</tr></thead>' % ''.join(rows[0])
            rows = rows[1:]
        body = ''.join('<tr>%s</tr>' % ''.join(r) for r in rows)
        return '<div class="table-wrap"><table>%s<tbody>%s</tbody></table></div>' % (head, body)

    def blockquote(self, c):
        paras = c.findall('para')
        if paras:
            first = paras[0]
            b = first[0] if len(first) else None
            if b is not None and b.tag == 'bold' and not (first.text or '').strip():
                word = plain(b).strip().rstrip(':').lower()
                if word in CALLOUTS:
                    kind = CALLOUTS[word]
                    # Markdown "**Note:** text" keeps the colon outside or inside the bold run.
                    first.remove(b)
                    tail = (b.tail or '').lstrip()
                    if tail.startswith(':'):
                        tail = tail[1:].lstrip()
                    first.text = tail
                    return self.callout(kind, word.capitalize(), self.group(self.items(c)))
        return '<blockquote>%s</blockquote>' % self.group(self.items(c))

    def callout(self, kind, title, inner):
        return ('<div class="callout callout-%s"><div class="callout-title">%s</div>%s</div>' %
                (kind, esc(title), inner))

    def simplesect(self, c):
        kind = c.get('kind')
        inner = self.group(self.items(c))
        if kind in CALLOUTS:
            return self.callout(CALLOUTS[kind], {'attention': 'Warning'}.get(kind, kind.capitalize()), inner)
        label = {'return': 'Returns', 'see': 'See also', 'sa': 'See also', 'since': 'Since',
                 'remark': 'Remark', 'pre': 'Precondition', 'post': 'Postcondition',
                 'par': 'Note', 'rcs': 'Revision', 'author': 'Author', 'version': 'Version',
                 'date': 'Date', 'copyright': 'Copyright', 'invariant': 'Invariant',
                 'user': 'User', 'warning': 'Warning'}.get(kind, kind.capitalize())
        return '<div class="simplesect"><div class="simplesect-title">%s</div>%s</div>' % (label, inner)

    def params(self, c):
        rows = []
        for it in c.findall('parameteritem'):
            names = ', '.join('<code>%s</code>' % esc(plain(n).strip())
                              for n in it.iter('parametername'))
            d = it.find('parameterdescription')
            rows.append('<tr><td class="pname">%s</td><td>%s</td></tr>' %
                        (names, self.group(self.items(d), tight=True)))
        label = {'param': 'Parameters', 'retval': 'Return values', 'exception': 'Exceptions',
                 'templateparam': 'Template parameters'}.get(c.get('kind'), 'Parameters')
        return ('<div class="params"><h4>%s</h4><div class="table-wrap"><table><tbody>%s</tbody>'
                '</table></div></div>' % (label, ''.join(rows)))

    # ---- grouping and code ------------------------------------------------------------------------------
    def group(self, items, tight=False, responses=False):
        """Join items; a run of code blocks in different languages becomes a tab group. Only
        alternatives are grouped: `json`, `text` and unlabelled blocks are output or data and never
        join a group. With responses, such a block right after a `bash` block is labelled Response."""
        out = []
        i = 0
        alt = lambda it: it[0] == 'code' and it[1] not in ('', 'text', 'json')
        while i < len(items):
            it = items[i]
            if alt(it):
                j = i
                langs = []
                while j < len(items) and alt(items[j]) and items[j][1] not in langs:
                    langs.append(items[j][1])
                    j += 1
                run = items[i:j]
                out.append(self.tabs(run) if len(run) > 1 else self.code(run[0][1], run[0][2]))
                i = j
                continue
            if it[0] == 'code':
                label = None
                if responses and i > 0 and items[i - 1][0] == 'code' and items[i - 1][1] == 'bash':
                    label = 'Response'
                out.append(self.code(it[1], it[2], label=label))
            else:
                out.append(it[1])
            i += 1
        s = ''.join(out)
        if tight and s.startswith('<p>') and s.count('<p>') == 1 and s.endswith('</p>'):
            return s[3:-4]
        return s

    def code(self, lang, text, cls='', label=None, copy_btn=True):
        label = label or LANG_NAMES.get(lang, lang or 'Text')
        btn = '<button class="copy" type="button" aria-label="Copy code">Copy</button>' if copy_btn else ''
        return ('<div class="code%s" data-lang="%s"><span class="code-lang">%s</span>%s'
                '<pre><code data-lang="%s">%s</code></pre></div>' %
                (cls, esc(lang), esc(label), btn, esc(lang), esc(text)))

    def tabs(self, run):
        bar = []
        panels = []
        for n, (_, lang, text) in enumerate(run):
            sel = 'true' if n == 0 else 'false'
            bar.append('<button type="button" role="tab" data-lang="%s" aria-selected="%s">%s</button>' %
                       (esc(lang), sel, esc(LANG_NAMES.get(lang, lang))))
            hidden = '' if n == 0 else ' hidden'
            panels.append('<div class="tabpanel" role="tabpanel" data-lang="%s"%s>%s</div>' %
                          (esc(lang), hidden, self.code(lang, text, ' in-tab', copy_btn=False)))
        return ('<div class="tabs" data-tabs><div class="tabbar"><div class="tablist" role="tablist">%s</div>'
                '<button class="copy" type="button" aria-label="Copy code">Copy</button></div>%s</div>' % (
                    ''.join(bar), ''.join(panels)))


def r_is_head(row):
    return any(en.get('thead') == 'yes' for en in row.findall('entry'))


def doxslug(i):
    k = i.find('_1')
    return i[k + 2:] if k >= 0 else i


def collect_anchors(page):
    """Own, per-page unique heading ids (slug of the title); Doxygen's ids are only kept to
    resolve links to them (anchor_map: Doxygen slug -> our id, '' for the page top)."""
    used = set()
    page.ids = {}
    page.anchor_map = {}
    first = True
    for e in page.cd.iter():
        if e.tag in ('sect1', 'sect2', 'sect3', 'sect4'):
            title = plain(e.find('title')).strip()
            if page.kind == 'home' and first and e.tag == 'sect1' and title == page.title:
                hid = ''
            else:
                base = slugify(title)
                hid = base
                n = 2
                while hid in used:
                    hid = '%s-%d' % (base, n)
                    n += 1
                used.add(hid)
            first = False
            page.ids[e.get('id')] = hid
            page.anchor_map[doxslug(e.get('id'))] = hid
        elif e.tag == 'anchor':
            page.anchor_map.setdefault(doxslug(e.get('id', '')), '')


def heading_id(page, sect):
    return page.ids[sect.get('id')]


def unwrap_title(dd, title):
    """README style: the first '# Title' is a sect1 with the page title; drop that level."""
    s1 = dd.findall('sect1')
    if len(s1) == 1 or (s1 and plain(s1[0].find('title')).strip() == title):
        first = s1[0]
        if plain(first.find('title')).strip() == title:
            new = ET.Element('detaileddescription')
            for c in dd:
                if c is not first:
                    new.append(c)
            # paragraphs of the unwrapped section come first, then its subsections
            pos = list(dd).index(first)
            inner = [c for c in first if c.tag != 'title']
            kids = [c for c in new]
            before = [c for c in dd if list(dd).index(c) < pos and c is not first]
            after = [c for c in dd if list(dd).index(c) > pos]
            new = ET.Element('detaileddescription')
            for c in before + inner + after:
                new.append(c)
            for c in new:
                if c.tag == 'sect2':
                    c.tag = 'sect1'
                    for d in c.iter():
                        if d.tag == 'sect3':
                            d.tag = 'sect2'
                        elif d.tag == 'sect4':
                            d.tag = 'sect3'
            return new
    return dd


def split_dd(dd):
    """(leading content elements, sect1 list); drops anchor-only paragraphs."""
    lead = []
    sects = []
    for c in dd:
        if c.tag == 'sect1':
            sects.append(c)
        elif c.tag == 'para':
            if len(c) and all(x.tag in ('anchor',) for x in c) and not (c.text or '').strip() \
                    and all(not (x.tail or '').strip() for x in c):
                continue
            if not len(c) and not (c.text or '').strip():
                continue
            lead.append(c)
        else:
            lead.append(c)
    return lead, sects


def first_summary(lead, rd):
    """Summary paragraph: the first plain paragraph (not code, list or table)."""
    for p in lead:
        if p.tag != 'para':
            continue
        if any(x.tag in BLOCK_TAGS for x in p):
            continue
        txt = plain(p).strip()
        if txt:
            return p, txt
    return None, ''


def render_sections(site, rd, page, sects, level=2, skip=()):
    out = []
    for s in sects:
        title = plain(s.find('title')).strip()
        if title in skip:
            continue
        hid = heading_id(page, s)
        page.headings.append((level, hid, title))
        tag = 'h%d' % level
        t_html = rd.inline(s.find('title'))
        out.append('<%s id="%s">%s<a class="anchor" href="#%s" aria-label="Link to this section">#</a></%s>' %
                   (tag, esc(hid), t_html, esc(hid), tag))
        items = []
        subs = []
        for c in s:
            if c.tag == 'title':
                continue
            if c.tag.startswith('sect'):
                subs.append(c)
                continue
            if c.tag == 'para':
                items.extend(rd.items(c))
            else:
                items.extend(rd.block(c))
        out.append(rd.group(items))
        if subs:
            for sub in subs:
                sub.tag = 'sect1'   # recurse with the next level
            out.append(render_sections(site, rd, page, subs, level + 1))
    return ''.join(out)


# ---- page bodies -------------------------------------------------------------------------------

def render_guide(site, rd, page):
    dd = page.cd.find('detaileddescription')
    if page.kind == 'home':
        dd = unwrap_title(dd, page.title)
    lead, sects = split_dd(dd)
    if page.kind == 'home':
        # the README's centered logo: the header already shows it
        lead = [c for c in lead
                if not (c.tag == 'para' and len(c) and all(x.tag == 'image' for x in c) and
                        not (c.text or '').strip())]
    sp, summ = first_summary(lead, rd)
    page.summary = summ
    items = []
    for c in lead:
        items.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
    # sect2 inside sect1 are rendered at level+1: retag nested levels before rendering
    body = rd.group(items)
    body += render_sections(site, rd, page, [retag(s) for s in sects])
    page.body = body


def retag(sect):
    """Flatten Doxygen's sect1/sect2/sect3 nesting into sect1 elements with nested sect1."""
    for c in list(sect):
        if c.tag.startswith('sect'):
            sect.remove(c)
            sect.append(retag(c))
    sect.tag = 'sect1'
    return sect


def render_entry(site, rd, page):
    dd = page.cd.find('detaileddescription')
    lead, sects = split_dd(dd)
    sp, summ = first_summary(lead, rd)
    if sp is None:
        site.error('reference entry has no summary paragraph before the first "##" section')
    page.summary = summ
    titles = [plain(s.find('title')).strip() for s in sects]
    last = -1
    for t in titles:
        if t not in ENTRY_SECTIONS:
            site.error('reference entry section "%s" is not one of: %s' % (t, ', '.join(ENTRY_SECTIONS)))
            continue
        k = ENTRY_SECTIONS.index(t)
        if k <= last:
            site.error('reference entry section "%s" is out of order or repeated (order: %s)' %
                       (t, ', '.join(ENTRY_SECTIONS)))
        last = max(last, k)
    by = {}
    for s in sects:
        by[plain(s.find('title')).strip()] = s

    def body_of(s):
        items = []
        for c in s:
            if c.tag == 'title':
                continue
            if c.tag.startswith('sect'):
                continue
            items.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
        subs = [retag(c) for c in s if c.tag.startswith('sect')]
        if subs:
            items.append(('html', render_sections(site, rd, page, subs, 3)))
        return items

    parts = []
    # lead: summary, then any further intro content
    if sp is not None:
        parts.append('<p class="lead">%s</p>' % rd.inline(sp))
    rest = []
    for c in lead:
        if c is sp:
            continue
        rest.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
    if rest:
        parts.append(rd.group(rest))

    # definition block: Syntax, Default, Valid in
    box = []
    if 'Syntax' in by:
        items = body_of(by['Syntax'])
        code = [i for i in items if i[0] == 'code']
        other = [i for i in items if i[0] != 'code']
        for i in code:
            box.append(rd.code(i[1] or 'scheme', i[2], ' syntax'))
        if other:
            box.append('<div class="refnote">%s</div>' % rd.group(other))
    facts = []
    for name in ('Default', 'Valid in'):
        if name in by:
            ss = by[name]
            # a list of values becomes chips, text stays text
            lists = [c for p in ss.findall('para') for c in p if c.tag == 'itemizedlist']
            if lists:
                chips = ''.join('<li>%s</li>' % rd.group(rd.items(li), tight=True)
                                for li in lists[0].findall('listitem'))
                val = '<ul class="chips">%s</ul>' % chips
            else:
                val = rd.group(body_of(ss), tight=True)
            facts.append('<div><dt>%s</dt><dd>%s</dd></div>' % (esc(name), val))
    if facts:
        box.append('<dl class="facts">%s</dl>' % ''.join(facts))
    if box:
        parts.append('<div class="refbox">%s</div>' % ''.join(box))

    def section(name, cls=''):
        if name not in by:
            return ''
        s = by[name]
        hid = heading_id(page, s)
        page.headings.append((2, hid, name))
        return ('<h2 id="%s">%s<a class="anchor" href="#%s" aria-label="Link to this section">#</a></h2>%s' %
                (esc(hid), esc(name), esc(hid), rd.group(body_of(s))))

    parts.append(section('Behaviour'))
    parts.append(section('Errors'))
    if 'Examples' in by:
        ex = by['Examples']
        hid = heading_id(page, ex)
        page.headings.append((2, hid, 'Examples'))
        parts.append('<aside class="ex" id="%s"><div class="ex-title">Examples</div>%s</aside>' %
                     (esc(hid), rd.group(body_of(ex), responses=True)))
    parts.append(section('See also'))
    page.body = ''.join(parts)


def error_rows(site, rd, page):
    """Message (HTML) of each bullet of the page's Errors section: the code span it starts with (with any
    ` / `-joined alternatives)."""
    rows = []
    sects = [s for s in split_dd(page.cd.find('detaileddescription'))[1]
             if plain(s.find('title')).strip() == 'Errors']
    for s in sects:
        for li in s.iter('listitem'):
            para = next(li.iter('para'), None)
            code = para[0] if para is not None and len(para) and para[0].tag == 'computeroutput' and \
                not (para.text or '').strip() else None
            if code is None:
                site.error('Errors bullet has no message in a code span: %s' % plain(li).strip()[:60])
                continue
            msg = rd.node(code)
            # alternatives written as `A` / `B` stay one message
            kids = list(para)
            k = kids.index(code) if code in kids else -1
            while k >= 0 and k + 1 < len(kids) and kids[k].tail and kids[k].tail.strip() == '/' and \
                    kids[k + 1].tag == 'computeroutput':
                k += 1
                msg += ' / ' + rd.node(kids[k])
            rows.append(msg)
    return rows


def render_errors(site, rd, page):
    """Error index: one table per area (reference index under the page's directory) built from
    the Errors sections of its entries, plus the guide pages named in errors_from."""
    root = posixpath.dirname(page.src) + '/'
    areas = []
    extra = [site.by_src[f] for f in page.errors_from if f in site.by_src]
    if extra:
        areas.append(('Top level', extra))
    for ix in site.pages:
        if ix.kind == 'index' and ix.src.startswith(root):
            areas.append((ix.title, [e for _, g in ix.groups for e in g]))
    out = []
    total = 0
    for title, pages in areas:
        trs = []
        for e in pages:
            site.cur = e
            rows = error_rows(site, rd, e)
            site.cur = page
            hid = next((h[1] for h in e.headings if h[2] == 'Errors'), '')
            href = esc(site.cur_root + e.url + ('#' + hid if hid else ''))
            for r in rows:
                trs.append('<tr><td>%s</td><td><a href="%s">%s</a></td></tr>' % (r, href, e.title_html))
        if not trs:
            continue
        total += len(trs)
        hid = slugify(title)
        page.headings.append((2, hid, title))
        out.append('<h2 id="%s">%s<a class="anchor" href="#%s" aria-label="Link to this section">#</a></h2>'
                   '<div class="table-wrap"><table><thead><tr><th>Message</th><th>Entry</th></tr></thead>'
                   '<tbody>%s</tbody></table></div>' % (esc(hid), esc(title), esc(hid), ''.join(trs)))
    if not total:
        site.error('error index found no Errors bullets')
    page.body += ''.join(out)


def render_index(site, rd, page):
    render_guide(site, rd, page)
    rows = []
    for gtitle, entries in page.groups:
        if gtitle:
            rows.append('<h2 class="idx-group" id="%s">%s</h2>' % (esc(slugify(gtitle)), esc(gtitle)))
            page.headings.append((2, slugify(gtitle), gtitle))
        lis = []
        for e in entries:
            q = (e.title + ' ' + e.summary).lower()
            lis.append('<li data-q="%s"><a href="%s"><span class="idx-name">%s</span>'
                       '<span class="idx-sum">%s</span></a></li>' %
                       (esc(q), esc(site.cur_root + e.url), e.title_html, esc(e.summary)))
        rows.append('<ul class="refindex">%s</ul>' % ''.join(lis))
    page.body += ('<div class="filterbox"><input type="search" class="filter" '
                  'placeholder="Filter %d entries" aria-label="Filter entries"></div>'
                  '<div class="refindex-wrap" data-filter-list>%s'
                  '<p class="no-match" hidden>No entry matches.</p></div>' %
                  (sum(len(g[1]) for g in page.groups), ''.join(rows)))


# ---- C API -------------------------------------------------------------------------------------

class Api:
    """The public header's symbols, grouped by the /* ---- Name ---- */ markers."""

    def __init__(self, site, header_name, title):
        self.s = site
        self.title = title
        hdr = Path(site.args.header)
        self.lines = hdr.read_text().splitlines()
        self.markers = []   # (line number, name)
        for n, l in enumerate(self.lines, 1):
            m = re.match(r'/\*\s*-{2,}\s*(.+?)\s*-{2,}\s*\*/', l)
            if m:
                self.markers.append((n, re.sub(r'\s*\(.*?\)', '', m.group(1))))
        self.fcd = None
        for f in sorted(site.xml.glob('*.xml')):
            if f.name == 'index.xml':
                continue
            for cd in ET.parse(f).getroot().findall('compounddef'):
                if cd.get('kind') == 'file' and cd.findtext('compoundname') == header_name:
                    self.fcd = cd
                    self.header = header_name
        if self.fcd is None:
            site.error('Doxygen XML has no file compound for %s' % header_name)
            return
        self.items = []     # dicts: kind, name, id, line, el, (group)
        self.collect()

    def group_of(self, line):
        name = self.markers[0][1]
        for n, g in self.markers:
            if n <= line:
                name = g
        return name

    def collect(self):
        sym = {}
        els = []
        seen_struct_names = set()
        for md in self.fcd.iter('memberdef'):
            kind = md.get('kind')
            name = md.findtext('name')
            line = int(md.find('location').get('line'))
            if kind == 'define':
                if name == 'MOGBUS_API':
                    continue
                els.append(dict(kind='define', name=name, line=line, el=md, id=md.get('id')))
            elif kind == 'function':
                els.append(dict(kind='function', name=name, line=line, el=md, id=md.get('id')))
            elif kind == 'enum':
                nm = '' if name.startswith('@') else name
                els.append(dict(kind='enum', name=nm or ('constants_%d' % line), line=line, el=md,
                                id=md.get('id'), anon=not nm))
            elif kind == 'typedef':
                ty = plain(md.find('type')).strip()
                if re.match(r'^(enum|struct)\s+' + re.escape(name) + r'$', ty):
                    # typedef of the same-named enum/struct: the enum/struct entry covers it
                    sym[md.get('id')] = name
                    continue
                els.append(dict(kind='typedef', name=name, line=line, el=md, id=md.get('id')))
        for ic in self.fcd.findall('innerclass'):
            rid = ic.get('refid')
            cd = ET.parse(site_xml(self.s, rid)).getroot().find('compounddef')
            line = int(cd.find('location').get('line'))
            nm = cd.findtext('compoundname')
            els.append(dict(kind='struct', name=nm, line=line, el=cd, id=rid))
        els.sort(key=lambda d: (d['line'], d['name']))
        for d in els:
            d['group'] = self.group_of(d['line'])
            d['slug'] = slugify(d['group'])
        self.items = els
        # group list in marker order
        order = []
        for _, g in self.markers:
            if g not in order:
                order.append(g)
        self.groups = [(g, slugify(g), [d for d in els if d['group'] == g]) for g in order]
        self.groups = [g for g in self.groups if g[2]]
        # symbol table: Doxygen ids -> url
        for gname, gslug, ds in self.groups:
            for d in ds:
                url = 'api/%s.html#%s' % (gslug, d['name'])
                d['url'] = url
                self.s.symbols[d['id']] = url
                if d['kind'] == 'enum':
                    for ev in d['el'].findall('enumvalue'):
                        self.s.symbols[ev.get('id')] = url
                if d['kind'] == 'struct':
                    for sd in d['el'].iter('memberdef'):
                        self.s.symbols[sd.get('id')] = url
        for tid, nm in sym.items():
            for d in els:
                if d['name'] == nm:
                    self.s.symbols[tid] = d['url']

    # ---- rendering -----------------------------------------------------------------------------
    def desc(self, rd, el):
        items = []
        for tag in ('briefdescription', 'detaileddescription'):
            e = el.find(tag)
            if e is None:
                continue
            for c in e:
                items.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
        return items

    def brief(self, el):
        b = el.find('briefdescription')
        t = plain(b).strip() if b is not None else ''
        if not t:
            d = el.find('detaileddescription')
            t = plain(d).strip() if d is not None else ''
            t = t.split('. ')[0] if t else ''
        return t

    def sig(self, rd, md):
        ty = rd.inline(md.find('type')).strip()
        name = md.findtext('name')
        params = []
        for p in md.findall('param'):
            pt = rd.inline(p.find('type')).strip() if p.find('type') is not None else ''
            dn = p.findtext('declname') or ''
            arr = p.findtext('array') or ''
            params.append(pt + ('' if pt.endswith('*') or not dn else ' ') + dn + arr)
        sep = '' if ty.endswith('*') else ' '
        plain_len = len(plain(md.find('type'))) + len(name) + sum(len(re.sub('<[^>]+>', '', x)) + 2 for x in params)
        if not params:
            args = '(void)'
            return '%s%s<b>%s</b>%s' % (ty, sep, esc(name), args)
        if plain_len < 70:
            return '%s%s<b>%s</b>(%s)' % (ty, sep, esc(name), ', '.join(params))
        return '%s%s<b>%s</b>(\n    %s)' % (ty, sep, esc(name), ',\n    '.join(params))

    def render_item(self, rd, d):
        el = d['el']
        k = d['kind']
        name = d['name']
        hid = esc(name)
        if k == 'function':
            sig = '<div class="code in-ex" data-lang="c"><span class="code-lang">C</span>' \
                  '<button class="copy" type="button" aria-label="Copy code">Copy</button>' \
                  '<pre><code data-lang="c" data-nohl>%s;</code></pre></div>' % self.sig(rd, el)
            b = self.brief(el)
            det = el.find('detaileddescription')
            retnodes = []
            for para in det.iter('para'):
                for ss in list(para):
                    if ss.tag == 'simplesect' and ss.get('kind') == 'return':
                        para.remove(ss)
                        retnodes.append(ss)
            items = []
            for c in det:
                items.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
            params = [i for i in items if i[0] == 'html' and i[1].startswith('<div class="params">')]
            text = [i for i in items if i not in params]
            rtxt = ''.join(rd.group(rd.items(r)) for r in retnodes)
            html_ = ('<section class="api-item api-fn" id="%s"><h3>%s<a class="anchor" href="#%s" '
                     'aria-label="Link to this function">#</a></h3>' % (hid, esc(name), hid))
            if b:
                html_ += '<p class="lead">%s</p>' % rd.inline(el.find('briefdescription')[0]).strip() \
                    if el.find('briefdescription') is not None and len(el.find('briefdescription')) else \
                    '<p class="lead">%s</p>' % esc(b)
            html_ += '<aside class="ex">%s</aside>' % sig
            html_ += rd.group(text)
            html_ += rd.group(params)
            if rtxt:
                html_ += '<div class="returns"><h4>Returns</h4>%s</div>' % rtxt
            html_ += '</section>'
            return html_
        if k == 'define':
            val = (el.findtext('initializer') or '').strip()
            code = '#define %s%s' % (name, (' ' + val) if val else '')
            return ('<section class="api-item" id="%s"><h3>%s<a class="anchor" href="#%s">#</a></h3>%s'
                    '<aside class="ex">%s</aside></section>' %
                    (hid, esc(name), hid, rd.group(self.desc(rd, el)), rd.code('c', code)))
        if k == 'typedef':
            return ('<section class="api-item" id="%s"><h3>%s<a class="anchor" href="#%s">#</a></h3>%s'
                    '<aside class="ex">%s</aside></section>' %
                    (hid, esc(name), hid, rd.group(self.desc(rd, el)),
                     rd.code('c', plain(el.find('definition')).strip() + plain(el.find('argsstring')) + ';')))
        if k == 'enum':
            rows = []
            for ev in el.findall('enumvalue'):
                dsc = []
                for tag in ('briefdescription', 'detaileddescription'):
                    e = ev.find(tag)
                    for c in (e if e is not None else []):
                        dsc.extend(rd.items(c))
                init = plain(ev.find('initializer')).strip() if ev.find('initializer') is not None else ''
                rows.append('<tr><td class="pname"><code>%s</code></td><td>%s</td></tr>' %
                            (esc(ev.findtext('name')), rd.group(dsc, tight=True)))
            title = 'enum ' + name if not d.get('anon') else 'Constants'
            return ('<section class="api-item" id="%s"><h3>%s<a class="anchor" href="#%s">#</a></h3>%s'
                    '<div class="table-wrap"><table><thead><tr><th>Value</th><th>Meaning</th></tr></thead>'
                    '<tbody>%s</tbody></table></div></section>' %
                    (hid, esc(title), hid, rd.group(self.desc(rd, el)), ''.join(rows)))
        if k == 'struct':
            rows = self.struct_rows(rd, el, '')
            return ('<section class="api-item" id="%s"><h3>struct %s<a class="anchor" href="#%s">#</a></h3>%s'
                    '<div class="table-wrap"><table><thead><tr><th>Field</th><th>Type</th><th>Meaning</th></tr>'
                    '</thead><tbody>%s</tbody></table></div></section>' %
                    (hid, esc(name), hid, rd.group(self.desc(rd, el)), ''.join(rows)))
        return ''

    def struct_rows(self, rd, cd, prefix):
        rows = []
        for md in cd.iter('memberdef'):
            if md.get('kind') != 'variable':
                continue
            name = prefix + md.findtext('name')
            ty = plain(md.find('type')).strip()
            arr = md.findtext('argsstring') or ''
            if '@' in ty:
                inner = None
                for ic in cd.findall('innerclass'):
                    cdi = ET.parse(site_xml(self.s, ic.get('refid'))).getroot().find('compounddef')
                    if md.findtext('name') in cdi.findtext('compoundname'):
                        inner = cdi
                dsc = []
                for tag in ('briefdescription', 'detaileddescription'):
                    e = md.find(tag)
                    for c in (e if e is not None else []):
                        dsc.extend(rd.items(c))
                rows.append('<tr><td class="pname"><code>%s</code></td><td><code>struct</code></td><td>%s</td></tr>' %
                            (esc(name), rd.group(dsc, tight=True)))
                if inner is not None:
                    rows.extend(self.struct_rows(rd, inner, name + '.'))
                continue
            dsc = []
            for tag in ('briefdescription', 'detaileddescription'):
                e = md.find(tag)
                for c in (e if e is not None else []):
                    dsc.extend(rd.items(c))
            rows.append('<tr><td class="pname"><code>%s%s</code></td><td><code>%s</code></td><td>%s</td></tr>' %
                        (esc(name), esc(arr), rd.inline(md.find('type')).strip(), rd.group(dsc, tight=True)))
        return rows


def site_xml(site, refid):
    return site.xml / (refid + '.xml')


def make_api_pages(site, rd):
    pages = []
    for cfg in site.apis:
        if cfg['py']:
            api = PyApi(site, cfg)
            pages.extend(api.pages(rd))
            continue
        header = Path(cfg['file']).name
        site.args.header = str(ROOT / cfg['file']) if not Path(cfg['file']).is_absolute() else cfg['file']
        api = Api(site, header, cfg['title'])
        if api.fcd is None:
            continue
        cfg['obj'] = api
        over = Page(src=cfg['file'], kind='api', section=cfg['section'], title=cfg['title'],
                    url='api/index.html', api=cfg)
        over.cd = api.fcd
        pages.append(over)
        for gname, gslug, ds in api.groups:
            p = Page(src=cfg['file'], kind='api', section=cfg['title'], title=gname,
                     url='api/%s.html' % gslug, api=cfg)
            p.ds = ds
            pages.append(p)
        over.groups = api.groups
    return pages


def render_api(site, rd, page):
    if page.api['py']:
        page.api['obj'].render(rd, page)
        return
    api = page.api['obj']
    if page.url == 'api/index.html':
        # overview: the header's file documentation, then a symbol list per group
        items = []
        for tag in ('briefdescription', 'detaileddescription'):
            e = api.fcd.find(tag)
            for c in (e if e is not None else []):
                items.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
        page.summary = api.brief(api.fcd) or 'Public C API of the mogbus library.'
        body = rd.group(items)
        for gname, gslug, ds in api.groups:
            body += '<h2 id="%s">%s</h2>' % (esc(gslug), esc(gname))
            page.headings.append((2, gslug, gname))
            rows = []
            for d in ds:
                if d['kind'] == 'enum' and d.get('anon'):
                    continue
                kindword = {'function': 'function', 'struct': 'struct', 'enum': 'enum',
                            'define': 'macro', 'typedef': 'type'}[d['kind']]
                rows.append('<tr><td class="pname"><a href="%s"><code>%s</code></a></td><td>%s</td><td>%s</td></tr>' %
                            (esc(site.cur_root + d['url']), esc(d['name']), kindword, esc(api.brief(d['el']))))
            body += ('<div class="table-wrap"><table><thead><tr><th>Name</th><th>Kind</th><th>Summary</th></tr></thead>'
                     '<tbody>%s</tbody></table></div>' % ''.join(rows))
        page.body = body
        return
    ds = page.ds
    page.summary = 'C API: %s.' % page.title
    chips = ''.join('<li><a href="#%s"><code>%s</code></a></li>' % (esc(d['name']), esc(d['name']))
                    for d in ds if not (d['kind'] == 'enum' and d.get('anon')))
    page.body = '<ul class="chips jump">%s</ul>' % chips
    defs = [d for d in ds if d['kind'] == 'define']
    if defs:
        rows = []
        for d in defs:
            val = (d['el'].findtext('initializer') or '').strip()
            rows.append('<tr id="%s"><td class="pname"><code>%s</code></td><td><code>%s</code></td><td>%s</td></tr>' %
                        (esc(d['name']), esc(d['name']), esc(val), rd.group(api.desc(rd, d['el']), tight=True)))
        page.body += ('<h3 class="macros">Macros</h3><div class="table-wrap"><table><thead><tr><th>Name</th>'
                      '<th>Value</th><th>Meaning</th></tr></thead><tbody>%s</tbody></table></div>' % ''.join(rows))
    for d in ds:
        if d['kind'] == 'define':
            continue
        page.body += api.render_item(rd, d)
        page.headings.append((3, d['name'], d['name']))
        page.anchors.add(d['name'])


# ---- Python API --------------------------------------------------------------------------------------

class PyApi:
    """The public classes and functions of a Python module. Names, signatures, fields and enum values
    come from the source (ast); descriptions come from Doxygen's XML of the docstrings."""

    def __init__(self, site, cfg):
        self.s = site
        self.cfg = cfg
        cfg['obj'] = self
        path = ROOT / cfg['file']
        self.pkg = path.parent.name
        self.mod = path.stem
        self.qual = '%s::%s' % (self.pkg, self.mod)
        self.tree = ast.parse(path.read_text())
        self.cds = {}
        for f in sorted(site.xml.glob('*.xml')):
            if not f.name.startswith(('class' + self.pkg, 'namespace' + self.pkg, self.mod + '_8py')):
                continue
            for cd in ET.parse(f).getroot().findall('compounddef'):
                self.cds[cd.findtext('compoundname')] = cd
        self.fcd = self.cds.get(self.mod + '.py')
        self.nscd = self.cds.get(self.qual)
        if self.fcd is None or self.nscd is None:
            site.error('Doxygen XML has no compound for %s' % cfg['file'])
        self.items = {}
        self.order = []
        self.collect()

    # ---- source analysis ------------------------------------------------------------------------
    @staticmethod
    def public(name):
        return not name.startswith('_')

    @staticmethod
    def decorators(node):
        return [ast.unparse(d).split('(')[0].split('.')[-1] for d in node.decorator_list]

    def collect(self):
        for node in self.tree.body:
            if isinstance(node, ast.ClassDef) and self.public(node.name):
                bases = [ast.unparse(b) for b in node.bases]
                deco = self.decorators(node)
                if any(b.split('.')[-1].endswith('Enum') for b in bases):
                    kind = 'enum'
                elif 'dataclass' in deco:
                    kind = 'dataclass'
                elif any(b.split('.')[-1] == 'NamedTuple' for b in bases):
                    kind = 'namedtuple'
                elif any(b.endswith('Exception') for b in bases):
                    kind = 'exception'
                else:
                    kind = 'class'
                it = dict(kind=kind, name=node.name, node=node, bases=bases, members=[], fields=[], values=[])
                for sub in node.body:
                    if isinstance(sub, ast.FunctionDef) and self.public(sub.name):
                        dec = self.decorators(sub)
                        mk = 'property' if 'property' in dec else 'classmethod' if 'classmethod' in dec else 'method'
                        it['members'].append(dict(kind=mk, name=sub.name, node=sub))
                    elif isinstance(sub, ast.AnnAssign) and kind in ('dataclass', 'namedtuple') and \
                            isinstance(sub.target, ast.Name) and self.public(sub.target.id):
                        it['fields'].append(sub)
                    elif isinstance(sub, ast.Assign) and isinstance(sub.targets[0], ast.Name) and \
                            self.public(sub.targets[0].id):
                        if kind == 'enum':
                            it['values'].append((sub.targets[0].id, ast.unparse(sub.value)))
                        else:
                            it['members'].append(dict(kind='attribute', name=sub.targets[0].id, node=sub))
                self.items[node.name] = it
                self.order.append(node.name)
            elif isinstance(node, ast.FunctionDef) and self.public(node.name):
                self.items[node.name] = dict(kind='function', name=node.name, node=node, members=[])
                self.order.append(node.name)

    def sig_args(self, node, skip):
        a = node.args
        out = []

        def one(arg, default):
            t = arg.arg
            if arg.annotation is not None:
                t += ': ' + ast.unparse(arg.annotation)
            if default is not None:
                t += (' = ' if arg.annotation is not None else '=') + ast.unparse(default)
            return t

        pos = a.posonlyargs + a.args
        defs = [None] * (len(pos) - len(a.defaults)) + list(a.defaults)
        for i, (arg, d) in enumerate(zip(pos, defs)):
            if i < skip:
                continue
            out.append(one(arg, d))
        if a.vararg:
            out.append('*' + one(a.vararg, None))
        elif a.kwonlyargs:
            out.append('*')
        for arg, d in zip(a.kwonlyargs, a.kw_defaults):
            out.append(one(arg, d))
        if a.kwarg:
            out.append('**' + one(a.kwarg, None))
        return out

    def param_names(self, node, skip):
        a = node.args
        names = [x.arg for x in (a.posonlyargs + a.args)][skip:] + [x.arg for x in a.kwonlyargs]
        if a.vararg:
            names.append(a.vararg.arg)
        if a.kwarg:
            names.append(a.kwarg.arg)
        return names

    def sig(self, name, node, skip=0, prefix=''):
        args = self.sig_args(node, skip)
        ret = ' -> ' + ast.unparse(node.returns) if node.returns is not None else ''
        one = '%s(%s)%s' % (name, ', '.join(args), ret)
        if len(one) > 72 and args:
            one = '%s(\n    %s)%s' % (name, ',\n    '.join(args), ret)
        return prefix + one

    # ---- Doxygen side ---------------------------------------------------------------------------
    def member(self, cd, name):
        found = [md for md in cd.iter('memberdef') if md.findtext('name') == name]
        # Doxygen also lists the self.NAME assignments of a method body as variables of the same name.
        return next((md for md in found if md.get('kind') == 'function'), found[0] if found else None)

    def split(self, rd, el, what, need_brief=True):
        """(brief html, plain brief, text items, parameter lists, returns html) of a described element."""
        b = el.find('briefdescription')
        bp = b.find('para') if b is not None else None
        brief = rd.inline(bp).strip() if bp is not None else ''
        plain_brief = plain(bp).strip() if bp is not None else ''
        if not brief and need_brief:
            self.s.error('%s: no docstring (a @brief line is required)' % what)
        det = copy.deepcopy(el.find('detaileddescription')) if el.find('detaileddescription') is not None \
            else ET.Element('detaileddescription')
        lists, rets = [], []
        for para in list(det.iter('para')):
            for ch in list(para):
                if ch.tag == 'parameterlist':
                    para.remove(ch)
                    lists.append(ch)
                elif ch.tag == 'simplesect' and ch.get('kind') == 'return':
                    para.remove(ch)
                    rets.append(ch)
        items = []
        for c in det:
            items.extend(rd.items(c) if c.tag == 'para' else rd.block(c))
        rtxt = ''.join(rd.group(rd.items(r)) for r in rets)
        return brief, plain_brief, items, lists, rtxt

    def check_params(self, what, lists, wanted):
        have = [plain(n).strip() for pl in lists if pl.get('kind') == 'param' for n in pl.iter('parametername')]
        if sorted(have) != sorted(wanted):
            self.s.error('%s: docstring parameters %s do not match the signature %s' %
                         (what, have, wanted))

    def tables(self, rd, lists):
        return ''.join(rd.params(pl) for pl in lists if pl.get('kind') == 'param') + \
               ''.join(rd.params(pl) for pl in lists if pl.get('kind') != 'param')

    # ---- pages ----------------------------------------------------------------------------------
    def pages(self, rd):
        cfg = self.cfg
        pages = []
        seen = set()
        groups = []
        for g in cfg['groups']:
            for n in g['items']:
                if n not in self.items:
                    self.s.error('%s: group %r lists %r, which is not a public name of %s' %
                                 (cfg['title'], g['title'], n, cfg['file']))
                elif n in seen:
                    self.s.error('%s: %r is listed twice' % (cfg['title'], n))
                seen.add(n)
            groups.append((g['title'], slugify(g['title']), [n for n in g['items'] if n in self.items]))
        for n in self.order:
            if n not in seen:
                self.s.error('%s: public name %r of %s is in no group of the manifest' % (cfg['title'], n, cfg['file']))
        self.groups = groups
        base = 'pyapi'
        for cd in (self.fcd, self.nscd):
            if cd is not None:
                self.s.symbols[cd.get('id')] = base + '/index.html'
        over = Page(src='docs/user/python/_', kind='api', section=cfg['section'], title=cfg['title'],
                    url='%s/index.html' % base, api=cfg)
        pages.append(over)
        for title, slug, names in groups:
            p = Page(src='docs/user/python/_', kind='api', section=cfg['title'], title=title,
                     url='%s/%s.html' % (base, slug), api=cfg)
            p.names = names
            pages.append(p)
            for n in names:
                it = self.items[n]
                url = '%s/%s.html#' % (base, slug)
                it['url'] = url + n
                cd = self.cds.get('%s::%s' % (self.qual, n))
                if cd is not None:
                    self.s.symbols[cd.get('id')] = url + n
                    members = [m['name'] for m in it['members']] + [v[0] for v in it['values']]
                    for md in cd.iter('memberdef'):
                        mn = md.findtext('name')
                        self.s.symbols[md.get('id')] = url + n + ('.' + mn if mn in members else '')
                elif self.nscd is not None:
                    md = self.member(self.nscd, n)
                    if md is not None:
                        self.s.symbols[md.get('id')] = url + n
        return pages

    def desc_cd(self, it):
        if it['kind'] == 'function':
            return self.member(self.nscd, it['name']) if self.nscd is not None else None
        return self.cds.get('%s::%s' % (self.qual, it['name']))

    def kindword(self, it):
        return {'function': 'function', 'enum': 'enum', 'dataclass': 'class', 'namedtuple': 'class',
                'exception': 'exception', 'class': 'class'}[it['kind']]

    def render(self, rd, page):
        if page.url.endswith('index.html'):
            self.render_overview(rd, page)
            return
        page.summary = 'Python API: %s.' % page.title
        names = page.names
        page.body = '<ul class="chips jump">%s</ul>' % ''.join(
            '<li><a href="#%s"><code>%s</code></a></li>' % (esc(n), esc(n)) for n in names)
        for n in names:
            page.body += self.render_item(rd, self.items[n])
            page.headings.append((3, n, n))
            page.anchors.add(n)

    def render_overview(self, rd, page):
        fcd = self.fcd
        b = fcd.find('briefdescription')
        page.summary = plain(b).strip() if b is not None else 'Python API of mogbus.'
        parts = []
        pend = []

        def flush():
            if pend:
                parts.append(rd.group(pend))
                pend.clear()

        def walk(el):
            for c in el:
                if c.tag == 'para':
                    pend.extend(rd.items(c))
                elif c.tag.startswith('sect'):
                    t = c.find('title')
                    if t is not None:
                        flush()
                        text = plain(t).strip()
                        hid = slugify(text)
                        parts.append('<h2 id="%s">%s</h2>' % (esc(hid), esc(text)))
                        page.headings.append((2, hid, text))
                    walk(c)
                elif c.tag != 'title':
                    pend.extend(rd.block(c))

        walk(fcd.find('detaileddescription'))
        flush()
        for title, slug, names in self.groups:
            parts.append('<h2 id="%s">%s</h2>' % (esc(slug), esc(title)))
            page.headings.append((2, slug, title))
            rows = []
            for n in names:
                it = self.items[n]
                cd = self.desc_cd(it)
                brief = ''
                if cd is not None and cd.find('briefdescription') is not None:
                    brief = plain(cd.find('briefdescription')).strip()
                rows.append('<tr><td class="pname"><a href="%s"><code>%s</code></a></td><td>%s</td><td>%s</td></tr>' %
                            (esc(self.s.cur_root + it['url']), esc(n), self.kindword(it), esc(brief)))
            parts.append('<div class="table-wrap"><table><thead><tr><th>Name</th><th>Kind</th><th>Summary</th>'
                         '</tr></thead><tbody>%s</tbody></table></div>' % ''.join(rows))
        page.body = ''.join(parts)

    def sigblock(self, text):
        return ('<aside class="ex"><div class="code in-ex" data-lang="python"><span class="code-lang">Python</span>'
                '<button class="copy" type="button" aria-label="Copy code">Copy</button>'
                '<pre><code data-lang="python" data-nohl>%s</code></pre></div></aside>' % esc(text))

    def section(self, ident, title, brief, sig, body, cls='api-item api-fn'):
        return ('<section class="%s" id="%s"><h3>%s<a class="anchor" href="#%s" aria-label="Link to this item">#</a></h3>'
                '%s%s%s</section>' % (cls, esc(ident), esc(title), esc(ident),
                                      '<p class="lead">%s</p>' % brief if brief else '', sig, body))

    def render_item(self, rd, it):
        name = it['name']
        cd = self.desc_cd(it)
        if cd is None:
            self.s.error('%s: Doxygen has no documentation for %s' % (self.cfg['file'], name))
            return ''
        if it['kind'] == 'function':
            return self.render_fn(rd, name, it['node'], cd, 0, '')
        brief, _, items, lists, rtxt = self.split(rd, cd, name)
        node = it['node']
        kind = it['kind']
        extra = ''
        if kind == 'enum':
            sigtxt = 'class %s(%s)' % (name, ', '.join(it['bases']))
            rows = []
            for vn, vv in it['values']:
                md = self.member(cd, vn)
                vb = ''
                if md is None or md.find('briefdescription') is None or not plain(md.find('briefdescription')).strip():
                    self.s.error('%s.%s: no doc comment (a ## line above the member)' % (name, vn))
                else:
                    vb = rd.group(rd.items(md.find('briefdescription')[0]), tight=True)
                rows.append('<tr><td class="pname"><code>%s</code></td><td><code>%s</code></td><td>%s</td></tr>' %
                            (esc(vn), esc(vv), vb))
            extra = ('<div class="table-wrap"><table><thead><tr><th>Member</th><th>Value</th><th>Meaning</th></tr>'
                     '</thead><tbody>%s</tbody></table></div>' % ''.join(rows))
        elif kind in ('dataclass', 'namedtuple'):
            fields = it['fields']
            args = []
            for f in fields:
                t = '%s: %s' % (f.target.id, ast.unparse(f.annotation))
                if f.value is not None:
                    t += ' = ' + ast.unparse(f.value)
                args.append(t)
            one = '%s(%s)' % (name, ', '.join(args))
            sigtxt = one if len(one) <= 72 else '%s(\n    %s)' % (name, ',\n    '.join(args))
            self.check_params(name, lists, [f.target.id for f in fields])
            docs = {}
            for pl in lists:
                if pl.get('kind') == 'param':
                    for pi in pl.findall('parameteritem'):
                        docs[plain(pi.find('parameternamelist')).strip()] = \
                            rd.group(rd.items(pi.find('parameterdescription')), tight=True)
            rows = []
            for f in fields:
                d = '' if f.value is None else ast.unparse(f.value)
                rows.append('<tr><td class="pname"><code>%s</code></td><td><code>%s</code></td><td>%s</td><td>%s</td></tr>' %
                            (esc(f.target.id), esc(ast.unparse(f.annotation)), '<code>%s</code>' % esc(d) if d else '',
                             docs.get(f.target.id, '')))
            extra = ('<div class="params"><h4>Fields</h4><div class="table-wrap"><table><thead><tr><th>Field</th>'
                     '<th>Type</th><th>Default</th><th>Meaning</th></tr></thead><tbody>%s</tbody></table></div></div>' %
                     ''.join(rows))
            lists = []
        else:
            sigtxt = 'class %s(%s)' % (name, ', '.join(it['bases'])) if it['bases'] else 'class %s' % name
            init = next((s for s in node.body if isinstance(s, ast.FunctionDef) and s.name == '__init__'), None)
            if init is not None:
                have = [plain(n).strip() for pl in lists if pl.get('kind') == 'param' for n in pl.iter('parametername')]
                wanted = self.param_names(init, 1)
                if have or not wanted:
                    self.check_params(name, lists, wanted)
                    sigtxt += '\n' + self.sig(name, init, 1)
        body = rd.group(items) + (self.tables(rd, lists) if lists else '') + extra
        # members: a summary table here, a section per member after it
        mrows = []
        msecs = []
        for m in it['members']:
            mname = m['name']
            md = self.member(cd, mname)
            qn = '%s.%s' % (name, mname)
            if md is None:
                self.s.error('%s: Doxygen has no documentation for %s' % (self.cfg['file'], qn))
                continue
            if m['kind'] == 'attribute':
                ab = plain(md.find('briefdescription')).strip() if md.find('briefdescription') is not None else ''
                if not ab:
                    self.s.error('%s: no doc comment (a ## line above the attribute)' % qn)
                val = ast.unparse(m['node'].value)
                mrows.append(('attribute', qn, mname, ab))
                msecs.append(self.section(qn, qn, esc(ab), self.sigblock('%s.%s = %s' % (name, mname, val)), ''))
                continue
            fsec = self.render_fn(rd, qn, m['node'], md, 1, m['kind'])
            _, pb, _, _, _ = self.split(rd, md, qn, need_brief=False)
            mrows.append((m['kind'], qn, mname, pb))
            msecs.append(fsec)
        if mrows:
            trs = ''.join('<tr><td class="pname"><a href="#%s"><code>%s</code></a></td><td>%s</td><td>%s</td></tr>' %
                          (esc(qn), esc(mn), k, esc(b)) for k, qn, mn, b in mrows)
            body += ('<div class="params"><h4>Members</h4><div class="table-wrap"><table><thead><tr><th>Name</th>'
                     '<th>Kind</th><th>Summary</th></tr></thead><tbody>%s</tbody></table></div></div>' % trs)
        title = '%s %s' % ({'enum': 'enum', 'exception': 'exception'}.get(kind, 'class'), name)
        return self.section(name, title, brief, self.sigblock(sigtxt), body, 'api-item') + ''.join(msecs)

    def render_fn(self, rd, qname, node, md, skip, mkind):
        brief, _, items, lists, rtxt = self.split(rd, md, qname)
        self.check_params(qname, lists, self.param_names(node, skip))
        deco = ''
        if mkind == 'property':
            deco = '@property\n'
        elif mkind == 'classmethod':
            deco = '@classmethod\n'
        elif 'contextmanager' in self.decorators(node):
            deco = '# context manager: use it in a with statement\n'
        if mkind == 'property':
            sigtxt = deco + qname + (' -> ' + ast.unparse(node.returns) if node.returns is not None else '')
        else:
            sigtxt = self.sig(qname, node, skip, deco)
        body = rd.group(items) + self.tables(rd, lists)
        if rtxt:
            body += '<div class="returns"><h4>Returns</h4>%s</div>' % rtxt
        return self.section(qname, qname, brief, self.sigblock(sigtxt), body)


# ---- layout ----------------------------------------------------------------------------------------

def nav_html(site, cur):
    root = site.cur_root
    out = []
    for stitle, nodes in site.nav:
        if stitle:
            out.append('<div class="nav-section">%s</div>' % esc(stitle))
        out.append('<ul>')
        for n in nodes:
            if 'api' in n:
                api_pages = [p for p in site.pages if p.kind == 'api' and p.api['idx'] == n['api']]
                parent = api_pages[0] if api_pages else None
                if parent is None:
                    continue
                kids = api_pages[1:]
                open_ = ' open' if cur.kind == 'api' and cur.api['idx'] == n['api'] else ''
                out.append('<li><details%s><summary><a href="%s"%s>%s</a></summary><ul>' %
                           (open_, esc(root + parent.url), cur_attr(cur, parent), esc(site.apis[n['api']]['title'])))
                for k in kids:
                    out.append('<li><a href="%s"%s>%s</a></li>' % (esc(root + k.url), cur_attr(cur, k), esc(k.title)))
                out.append('</ul></details></li>')
                continue
            p = n['page']
            kids = n.get('children')
            if kids:
                open_ = ' open' if (cur is p or cur in kids) else ''
                out.append('<li><details%s><summary><a href="%s"%s>%s</a></summary><ul>' %
                           (open_, esc(root + p.url), cur_attr(cur, p), p.title_html or esc(p.title)))
                for k in kids:
                    out.append('<li><a href="%s"%s>%s</a></li>' %
                               (esc(root + k.url), cur_attr(cur, k), k.title_html or esc(k.title)))
                out.append('</ul></details></li>')
            else:
                out.append('<li><a href="%s"%s>%s</a></li>' %
                           (esc(root + p.url), cur_attr(cur, p), (getattr(p, 'nav_title', '') and esc(p.nav_title)) or p.title_html or esc(p.title)))
        out.append('</ul>')
    return ''.join(out)


def cur_attr(cur, p):
    return ' aria-current="page"' if cur is p else ''


def toc_html(page):
    items = [(l, i, t) for (l, i, t) in page.headings if l in (2, 3)]
    if len(items) < 2:
        return ''
    lis = ''.join('<li class="l%d"><a href="#%s">%s</a></li>' % (l, esc(i), esc(t)) for l, i, t in items)
    return '<aside class="toc" aria-label="On this page"><div class="toc-title">On this page</div><ul>%s</ul></aside>' % lis


TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<meta name="description" content="{desc}">
<script>try{{var t=localStorage.getItem('mogbus-theme');if(t)document.documentElement.setAttribute('data-theme',t)}}catch(e){{}}</script>
<link rel="icon" href="{root}assets/mogbus-logo.svg" type="image/svg+xml">
<link rel="stylesheet" href="{root}assets/style.css">
</head>
<body class="kind-{kind}" data-root="{root}">
<header class="topbar">
<button class="menu-btn" type="button" aria-label="Menu" aria-expanded="false" aria-controls="sidenav"><span></span></button>
<a class="brand" href="{root}index.html"><img src="{root}assets/mogbus-logo.svg" alt="" width="28" height="28"><span>mogbus</span><span class="brand-sub">docs</span></a>
<div class="search" role="search"><input type="search" id="q" placeholder="Search the docs" aria-label="Search the docs" autocomplete="off" spellcheck="false"><kbd class="slash">/</kbd><ul id="results" class="results" hidden></ul></div>
<button class="theme-btn" type="button" aria-label="Toggle light and dark theme"></button>
</header>
<div class="shell">
<nav class="sidenav" id="sidenav" aria-label="Documentation">{nav}</nav>
<main id="main">
<article class="doc {cls}">
<div class="crumbs">{crumbs}</div>
<h1>{h1}</h1>
{body}
{pager}
</article>
</main>
</div>
<script src="{root}assets/search-index.js"></script>
<script src="{root}assets/site.js"></script>
</body>
</html>
"""


def crumbs_for(site, page):
    parts = []
    if page.kind == 'home':
        return ''
    if page.section:
        parts.append(esc(page.section))
    if page.parent is not None:
        parts = [esc(page.parent.section), '<a href="%s">%s</a>' % (esc(site.cur_root + page.parent.url), page.parent.title_html)]
    return ' <span>/</span> '.join(parts)


def flat_order(site):
    out = []
    for _, nodes in site.nav:
        for n in nodes:
            if 'api' in n:
                out.extend(p for p in site.pages if p.kind == 'api' and p.api['idx'] == n['api'])
            else:
                out.append(n['page'])
                out.extend(n.get('children') or [])
    return out


def write_page(site, page, order):
    site.cur = page
    depth = page.url.count('/')
    site.cur_root = '../' * depth
    root = site.cur_root
    pager = ''
    if page in order:
        i = order.index(page)
        prev = order[i - 1] if i > 0 else None
        nxt = order[i + 1] if i + 1 < len(order) else None
        pager = '<nav class="pager">%s%s</nav>' % (
            '<a class="prev" href="%s"><span>Previous</span>%s</a>' % (esc(root + prev.url), prev.title_html or esc(prev.title)) if prev else '<span></span>',
            '<a class="next" href="%s"><span>Next</span>%s</a>' % (esc(root + nxt.url), nxt.title_html or esc(nxt.title)) if nxt else '')
    entry = page.kind in ('entry', 'api')
    cls = {'entry': 'entry', 'api': 'apipage'}.get(page.kind, 'guide')
    toc = '' if entry else toc_html(page)
    body = page.body
    if toc:
        # the TOC sits right after the title so it shares the grid's right column
        body = toc + body
    html_ = TEMPLATE.format(
        title=esc(('%s · mogbus' % page.title) if page.kind != 'home' else 'mogbus documentation'),
        desc=esc(page.summary[:200]), root=root, kind=page.kind, nav=nav_html(site, page),
        cls=cls, crumbs=crumbs_for(site, page),
        h1=page.title_html or esc(page.title), body=body, pager=pager)
    path = site.out / page.url
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(html_)


def search_index(site):
    recs = []
    for p in site.pages:
        recs.append({'t': p.title, 's': p.section, 'd': p.summary[:200], 'u': p.url,
                     'h': [[i, t] for (l, i, t) in p.headings if l == 2 or p.kind == 'api']})
    return recs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xml', default=str(ROOT / 'build/docs/xml'))
    ap.add_argument('--out', default=str(ROOT / 'build/docs'))
    ap.add_argument('--manifest', default=str(ROOT / 'docs/site.json'))
    ap.add_argument('--no-internals', action='store_true',
                    help='do not link to or check the Doxygen HTML in OUT/internals')
    ap.add_argument('--assets', default=str(ROOT / 'docs/assets'))
    args = ap.parse_args()
    args.header = ''

    site = Site(args)
    site.cur_root = ''
    try:
        site.load_index()
        found = site.load_pages()
        site.build_nav(found)
        seen = {}
        for p in site.pages:
            if p.url in seen:
                site.error('%s and %s would both be written to %s' % (seen[p.url], p.src, p.url))
            seen[p.url] = p.src
        if site.errors:
            raise Fail()
        rd = Render(site)
        for p in site.pages:
            collect_anchors(p)
            p.anchors = p.anchor_map
        api_pages = make_api_pages(site, rd)
        for p in api_pages:
            site.pages.append(p)
        # titles (inline HTML) first: index lists use them
        for p in site.pages:
            if p.kind != 'api':
                site.cur = p
                site.cur_root = '../' * p.url.count('/')
                t = p.cd.find('title')
                p.title_html = title_html(plain(t).strip()) if t is not None and p.title == clean_title(plain(t).strip()) else esc(p.title)
        for p in site.pages:
            site.cur = p
            site.cur_root = '../' * p.url.count('/')
            if p.kind in ('home', 'guide'):
                render_guide(site, rd, p)
            elif p.kind == 'entry':
                render_entry(site, rd, p)
        for p in site.pages:
            site.cur = p
            site.cur_root = '../' * p.url.count('/')
            if p.errors_from is not None:
                render_errors(site, rd, p)
            if p.kind == 'index':
                render_index(site, rd, p)
            elif p.kind == 'api':
                render_api(site, rd, p)
        if site.errors:
            raise Fail()
        # write
        order = flat_order(site)
        site.out.mkdir(parents=True, exist_ok=True)
        (site.out / 'assets').mkdir(exist_ok=True)
        for f in sorted(THEME.iterdir()):
            shutil.copyfile(f, site.out / 'assets' / f.name)
        ad = Path(args.assets)
        if ad.is_dir():
            for f in sorted(ad.iterdir()):
                if f.is_file():
                    shutil.copyfile(f, site.out / 'assets' / f.name)
        for p in site.pages:
            write_page(site, p, order)
        idx = search_index(site)
        data = json.dumps(idx, sort_keys=True, separators=(',', ':'), ensure_ascii=False)
        (site.out / 'assets' / 'search-index.json').write_text(data)
        (site.out / 'assets' / 'search-index.js').write_text('window.MOGBUS_INDEX=%s;\n' % data)
        if site.errors:
            raise Fail()
    except Fail:
        pass
    if site.errors:
        for e in sorted(set(site.errors)):
            print('docs: error: ' + e, file=sys.stderr)
        sys.exit(1)
    print('docs: %d pages written to %s' % (len(site.pages), site.out))


if __name__ == '__main__':
    main()
