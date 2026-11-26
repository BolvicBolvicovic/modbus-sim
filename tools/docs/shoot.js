// Screenshots of generated pages (run in the Playwright container by shoot.sh).
// Usage: node shoot.js SITE_DIR OUT_DIR spec...   spec = page.html:WIDTHxHEIGHT[:dark][:full][:#anchor]
const { chromium } = require('playwright-core');

(async () => {
  const [site, out, ...specs] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  for (const spec of specs) {
    const [page, size, ...flags] = spec.split(':');
    const [w, h] = size.split('x').map(Number);
    const dark = flags.includes('dark');
    const ctx = await browser.newContext({
      viewport: { width: w, height: h },
      colorScheme: dark ? 'dark' : 'light',
    });
    const p = await ctx.newPage();
    const anchor = flags.find((f) => f.startsWith('#')) || '';
    await p.goto('file://' + site + '/' + page + anchor);
    await p.waitForTimeout(300);
    const name = page.replace(/\//g, '_').replace(/\.html$/, '') + '-' + size + (dark ? '-dark' : '') + (anchor ? '-' + anchor.slice(1) : '');
    await p.screenshot({ path: out + '/' + name + '.png', fullPage: flags.includes('full') });
    await ctx.close();
  }
  await browser.close();
})();
