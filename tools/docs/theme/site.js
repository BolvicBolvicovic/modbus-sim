/* mogbus docs: theme toggle, menu, code (highlight, copy, language tabs), search, index filter,
   table-of-contents highlighting. Vanilla JS, no dependencies. */
(function () {
  'use strict';
  var doc = document;
  var root = doc.body.getAttribute('data-root') || '';

  function store(key, val) {
    try {
      if (val === undefined) { return localStorage.getItem(key); }
      localStorage.setItem(key, val);
    } catch (e) { /* storage may be unavailable */ }
    return null;
  }
  function $(sel, ctx) { return (ctx || doc).querySelector(sel); }
  function $$(sel, ctx) { return Array.prototype.slice.call((ctx || doc).querySelectorAll(sel)); }

  /* ---- theme ---- */
  var themeBtn = $('.theme-btn');
  if (themeBtn) {
    themeBtn.addEventListener('click', function () {
      var el = doc.documentElement;
      var cur = el.getAttribute('data-theme');
      if (!cur) {
        cur = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
      }
      var next = cur === 'dark' ? 'light' : 'dark';
      el.setAttribute('data-theme', next);
      store('mogbus-theme', next);
    });
  }

  /* ---- menu (narrow screens) ---- */
  var menu = $('.menu-btn');
  if (menu) {
    menu.addEventListener('click', function () {
      var open = doc.body.classList.toggle('nav-open');
      menu.setAttribute('aria-expanded', open ? 'true' : 'false');
    });
    $$('.sidenav a').forEach(function (a) {
      a.addEventListener('click', function () { doc.body.classList.remove('nav-open'); });
    });
  }
  var cur = $('.sidenav a[aria-current="page"]');
  if (cur && cur.scrollIntoView) { cur.scrollIntoView({ block: 'center' }); }

  /* ---- syntax highlighting: a tiny tokenizer per language ---- */
  var KW = {
    c: 'if else for while do switch case default break continue return goto sizeof typedef struct union enum static const extern volatile inline void char short int long float double unsigned signed bool true false NULL uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t',
    python: 'and as assert async await break class continue def del elif else except finally for from global if import in is lambda None nonlocal not or pass raise return try while with yield True False self',
    bash: 'if then else elif fi for while do done case esac function in echo export cd sudo set unset',
    scheme: 'define let lambda if cond else and or not quote begin'
  };
  function set(s) { var o = {}; s.split(' ').forEach(function (w) { o[w] = 1; }); return o; }
  var RULES = {
    c: [['c', /\/\/[^\n]*|\/\*[\s\S]*?\*\//], ['s', /"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/],
        ['a', /#\s*\w+/], ['n', /\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?)[uUlLfF]*\b/], ['w', /[A-Za-z_]\w*/]],
    python: [['c', /#[^\n]*/], ['s', /"""[\s\S]*?"""|'''[\s\S]*?'''|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/],
        ['n', /\b\d+(?:\.\d+)?\b/], ['w', /[A-Za-z_]\w*/]],
    bash: [['c', /(^|\s)#[^\n]*/], ['s', /"(?:\\.|[^"\\])*"|'[^']*'/], ['a', /\$\{?\w+\}?|\$\(/],
        ['k', /(^|\s)--?[A-Za-z][\w-]*/], ['n', /\b\d+\b/], ['w', /[A-Za-z_][\w-]*/]],
    json: [['s', /"(?:\\.|[^"\\\n])*"/], ['n', /-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b/], ['w', /\b(?:true|false|null)\b/]],
    scheme: [['c', /;[^\n]*/], ['s', /"(?:\\.|[^"\\])*"/], ['n', /#x[0-9a-fA-F]+|-?\b\d+(?:\.\d+)?\b/],
        ['a', /#[tf]\b/], ['f', /\((?=[A-Za-z][^\s()]*)/]]
  };
  RULES.yaml = RULES.python; RULES.ini = [['c', /[;#][^\n]*/], ['s', /"(?:\\.|[^"\\\n])*"/], ['k', /^\[[^\]]+\]/m], ['n', /\b\d+\b/]];
  RULES.powershell = RULES.python;

  function esc(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }

  function highlight(code) {
    var lang = code.getAttribute('data-lang');
    var text = code.textContent;
    var rules = RULES[lang];
    if (!rules) { return; }
    var kw = set(KW[lang] || '');
    var out = '';
    var i = 0;
    var re = rules.map(function (r) { return new RegExp(r[1].source, r[1].flags.replace('g', '') + 'y'); });
    var plainStart = 0;
    function flushPlain(upto) { if (upto > plainStart) { out += esc(text.slice(plainStart, upto)); } }
    while (i < text.length) {
      var hit = null;
      for (var r = 0; r < rules.length && !hit; r++) {
        re[r].lastIndex = i;
        var m = re[r].exec(text);
        if (m && m[0].length) { hit = [rules[r][0], m[0], r]; }
      }
      if (!hit) { i++; continue; }
      var cls = hit[0];
      var tok = hit[1];
      var lead = '';
      if (lang === 'bash' && (cls === 'c' || cls === 'k') && /^\s/.test(tok)) { lead = tok.match(/^\s/)[0]; tok = tok.slice(1); }
      if (cls === 'w') {
        if (lang === 'json') { cls = 'k'; }
        else if (kw[tok]) { cls = 'k'; }
        else if (lang === 'bash' && i === 0) { cls = 'f'; }
        else { i += tok.length; continue; }
      }
      if (lang === 'json' && cls === 's' && /^\s*:/.test(text.slice(i + hit[1].length))) { cls = 'a'; }
      if (lang === 'scheme' && cls === 'f') {
        var nm = /^[^\s()]+/.exec(text.slice(i + 1));
        flushPlain(i);
        out += '(<span class="tk-f">' + esc(nm ? nm[0] : '') + '</span>';
        i += 1 + (nm ? nm[0].length : 0);
        plainStart = i;
        continue;
      }
      flushPlain(i);
      out += esc(lead) + '<span class="tk-' + cls + '">' + esc(tok) + '</span>';
      i += lead.length + tok.length;
      plainStart = i;
    }
    flushPlain(text.length);
    code.innerHTML = out;
  }
  $$('pre code[data-lang]').forEach(function (c) {
    if (c.hasAttribute('data-nohl')) { return; }
    highlight(c);
  });

  /* ---- copy buttons ---- */
  doc.addEventListener('click', function (ev) {
    var b = ev.target.closest && ev.target.closest('.copy');
    if (!b) { return; }
    var block = b.closest('.tabs') ? $('.tabpanel:not([hidden]) pre', b.closest('.tabs')) : $('pre', b.parentNode);
    var text = block ? block.textContent : '';
    function done() { b.textContent = 'Copied'; b.classList.add('done'); setTimeout(function () { b.textContent = 'Copy'; b.classList.remove('done'); }, 1400); }
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, function () { });
    } else {
      var ta = doc.createElement('textarea');
      ta.value = text; doc.body.appendChild(ta); ta.select();
      try { doc.execCommand('copy'); done(); } catch (e) { /* ignore */ }
      doc.body.removeChild(ta);
    }
  });

  /* ---- language tabs ---- */
  function selectLang(tabs, lang) {
    var btns = $$('[role="tab"]', tabs);
    if (!btns.some(function (b) { return b.getAttribute('data-lang') === lang; })) { return false; }
    btns.forEach(function (b) { b.setAttribute('aria-selected', b.getAttribute('data-lang') === lang ? 'true' : 'false'); });
    $$('.tabpanel', tabs).forEach(function (p) { p.hidden = p.getAttribute('data-lang') !== lang; });
    return true;
  }
  var saved = store('mogbus-lang');
  if (saved) { $$('[data-tabs]').forEach(function (t) { selectLang(t, saved); }); }
  doc.addEventListener('click', function (ev) {
    var b = ev.target.closest && ev.target.closest('[role="tab"]');
    if (!b) { return; }
    var lang = b.getAttribute('data-lang');
    store('mogbus-lang', lang);
    $$('[data-tabs]').forEach(function (t) { selectLang(t, lang); });
  });

  /* ---- search ---- */
  var input = $('#q');
  var list = $('#results');
  var index = window.MOGBUS_INDEX || [];
  var sel = -1;
  var shown = [];

  function score(hay, terms) {
    var s = 0;
    for (var i = 0; i < terms.length; i++) {
      var p = hay.indexOf(terms[i]);
      if (p < 0) { return -1; }
      s += p === 0 ? 3 : 1;
    }
    return s;
  }
  function search(q) {
    var terms = q.toLowerCase().split(/\s+/).filter(Boolean);
    var res = [];
    if (!terms.length) { return res; }
    index.forEach(function (p) {
      var t = score(p.t.toLowerCase(), terms);
      var d = score((p.t + ' ' + p.d).toLowerCase(), terms);
      if (t >= 0) { res.push({ s: 100 + t, title: p.t, sec: p.s, sum: p.d, url: p.u }); }
      else if (d >= 0) { res.push({ s: 30 + d, title: p.t, sec: p.s, sum: p.d, url: p.u }); }
      p.h.forEach(function (h) {
        var k = score(h[1].toLowerCase(), terms);
        if (k >= 0) { res.push({ s: 60 + k, title: h[1], sec: p.t, sum: '', url: p.u + '#' + h[0] }); }
      });
    });
    res.sort(function (a, b) { return b.s - a.s || (a.title < b.title ? -1 : 1); });
    return res.slice(0, 12);
  }
  function show(q) {
    shown = search(q);
    sel = shown.length ? 0 : -1;
    if (!q.trim()) { list.hidden = true; return; }
    list.innerHTML = shown.length ? shown.map(function (r, i) {
      return '<li' + (i === 0 ? ' class="sel"' : '') + '><a href="' + root + esc(r.url) + '"><span class="r-title">' + esc(r.title) +
        '</span><span class="r-sec">' + esc(r.sec) + '</span>' + (r.sum ? '<span class="r-sum">' + esc(r.sum) + '</span>' : '') + '</a></li>';
    }).join('') : '<li class="none">No results</li>';
    list.hidden = false;
  }
  function move(d) {
    var items = $$('li', list);
    if (!shown.length) { return; }
    sel = (sel + d + shown.length) % shown.length;
    items.forEach(function (li, i) { li.classList.toggle('sel', i === sel); });
    if (items[sel].scrollIntoView) { items[sel].scrollIntoView({ block: 'nearest' }); }
  }
  if (input) {
    input.addEventListener('input', function () { show(input.value); });
    input.addEventListener('focus', function () { if (input.value) { show(input.value); } });
    input.addEventListener('keydown', function (ev) {
      if (ev.key === 'ArrowDown') { ev.preventDefault(); move(1); }
      else if (ev.key === 'ArrowUp') { ev.preventDefault(); move(-1); }
      else if (ev.key === 'Enter' && sel >= 0 && shown[sel]) { window.location.href = root + shown[sel].url; }
      else if (ev.key === 'Escape') { list.hidden = true; input.blur(); }
    });
    doc.addEventListener('click', function (ev) {
      if (!ev.target.closest('.search')) { list.hidden = true; }
    });
    doc.addEventListener('keydown', function (ev) {
      var t = ev.target;
      var typing = t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable);
      if (ev.key === '/' && !typing && !ev.ctrlKey && !ev.metaKey && !ev.altKey) {
        ev.preventDefault();
        input.focus();
        input.select();
      }
    });
  }

  /* ---- filterable reference index ---- */
  $$('[data-filter-list]').forEach(function (wrap) {
    var f = $('.filter');
    if (!f) { return; }
    var items = $$('li[data-q]', wrap);
    var none = $('.no-match', wrap);
    f.addEventListener('input', function () {
      var terms = f.value.toLowerCase().split(/\s+/).filter(Boolean);
      var any = false;
      items.forEach(function (li) {
        var q = li.getAttribute('data-q');
        var ok = terms.every(function (t) { return q.indexOf(t) >= 0; });
        li.hidden = !ok;
        if (ok) { any = true; }
      });
      $$('ul.refindex', wrap).forEach(function (ul) {
        var vis = $$('li', ul).some(function (li) { return !li.hidden; });
        ul.hidden = !vis;
        var h = ul.previousElementSibling;
        if (h && h.classList.contains('idx-group')) { h.hidden = !vis; }
      });
      if (none) { none.hidden = any; }
    });
  });

  /* ---- table of contents: highlight the heading in view ---- */
  var toc = $('.toc');
  if (toc) {
    var links = {};
    $$('a', toc).forEach(function (a) { links[decodeURIComponent(a.getAttribute('href').slice(1))] = a; });
    var heads = $$('.doc h2[id], .doc h3[id], .doc h4[id], .doc h5[id]').filter(function (h) { return links[h.id]; });
    var lit = [];
    var pending = false;
    var spy = function () {
      pending = false;
      /* every heading whose section (up to the next heading, the last one up to the end of the page) intersects the visible band */
      var top = $('.topbar') ? $('.topbar').getBoundingClientRect().bottom : 0;
      var bottom = window.innerHeight;
      var end = ($('.pager') || $('.doc')).getBoundingClientRect().top;
      var on = [];
      for (var i = 0; i < heads.length; i++) {
        var s = heads[i].getBoundingClientRect().top;
        var e = i + 1 < heads.length ? heads[i + 1].getBoundingClientRect().top : end;
        if (s < bottom && e > top) { on.push(i); }
      }
      if (!on.length && heads.length) {
        /* above the first heading: the first one; otherwise the last one passed */
        var k = 0;
        while (k + 1 < heads.length && heads[k + 1].getBoundingClientRect().top <= top) { k++; }
        on.push(k);
      }
      var next = on.map(function (i) { return links[heads[i].id]; });
      lit.forEach(function (a) { if (next.indexOf(a) < 0) { a.classList.remove('active', 'first', 'last'); } });
      next.forEach(function (a, n) {
        a.classList.add('active');
        a.classList.toggle('first', n === 0);
        a.classList.toggle('last', n === next.length - 1);
      });
      lit = next;
      if (next.length && toc.scrollHeight > toc.clientHeight) {
        var t = next[0].offsetTop, h = next[next.length - 1].offsetTop + next[next.length - 1].offsetHeight;
        if (t < toc.scrollTop || h > toc.scrollTop + toc.clientHeight) { toc.scrollTop = Math.max(0, t - 40); }
      }
    };
    var onScroll = function () { if (!pending) { pending = true; window.requestAnimationFrame(spy); } };
    window.addEventListener('scroll', onScroll, { passive: true });
    window.addEventListener('resize', onScroll);
    spy();
  }
})();
