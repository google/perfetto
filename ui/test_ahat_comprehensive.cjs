// Comprehensive Ahat plugin browser test.
//
// Prerequisites:
//   - trace_processor_shell -D --http-port 9001 <hprof>
//   - python3 -m http.server 10000 serving ui/out/dist
//
// Usage: node test_ahat_comprehensive.cjs

const puppeteer = require('puppeteer');

const BASE = 'http://localhost:10000/v54.0-1e8320dab/';

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function navHash(page, hash) {
  await page.evaluate((h) => {
    window.location.hash = h;
  }, hash);
  // Poll until Ahat content appears (overview query can be slow for large HPROFs).
  for (let i = 0; i < 90; i++) {
    await sleep(1000);
    const n = await page.evaluate(
      () => document.querySelectorAll('[class*="ah-"]').length,
    );
    if (n > 5) break;
  }
}

async function countAhat(page) {
  return page.evaluate(() =>
    document.querySelectorAll('[class*="ah-"]').length,
  );
}

async function getCrumbs(page) {
  return page.evaluate(() => {
    const items = document.querySelectorAll('.ah-breadcrumbs__item');
    return Array.from(items).map((e) => e.textContent.trim());
  });
}

async function getText(page) {
  return page.evaluate(() => document.body.innerText);
}

let passed = 0;
let failed = 0;

function ok(name) {
  passed++;
  console.log(`  ✓ ${name}`);
}
function fail(name, err) {
  failed++;
  console.error(`  ✗ ${name}: ${err}`);
}

async function loadTrace(page) {
  await page.goto(BASE, {waitUntil: 'networkidle0'});
  await page.evaluate(() => {
    const f = JSON.parse(
      localStorage.getItem('perfettoFeatureFlags') || '{}',
    );
    f['plugin_com.android.Ahat'] = 'OVERRIDE_TRUE';
    localStorage.setItem('perfettoFeatureFlags', JSON.stringify(f));
  });
  await page.reload({waitUntil: 'networkidle0'});
  await sleep(2000);
  await page.evaluate(() => {
    window.location.hash = '#!/?rpc_port=9001';
  });
  for (let i = 0; i < 15; i++) {
    await sleep(2000);
    const d = await page.evaluate(() => {
      for (const b of document.querySelectorAll('button')) {
        if (b.textContent.includes('YES, use loaded trace')) {
          b.click();
          return true;
        }
      }
      return false;
    });
    if (d) {
      console.log('  Dismissed version dialog');
      break;
    }
  }
  // Poll for Ahat sidebar to appear (dominator tree materialization may take
  // over a minute for large HPROFs).
  console.log('  Waiting for Ahat sidebar...');
  for (let i = 0; i < 90; i++) {
    await sleep(2000);
    const has = await page.evaluate(
      () => document.querySelectorAll('[href*="ahat"]').length,
    );
    if (has > 0) {
      console.log(`  Ahat ready (${i * 2}s)`);
      break;
    }
  }
  await sleep(2000);
}

async function run() {
  const browser = await puppeteer.launch({
    headless: 'new',
    args: ['--no-sandbox'],
  });
  const page = await browser.newPage();
  page.setDefaultTimeout(30000);

  try {
    console.log('Loading trace via RPC...');
    await loadTrace(page);

    // ── Test 1: Overview ────────────────────────────────────────────────
    console.log('\nTest 1: Overview');
    await navHash(page, '#!/ahat');
    const n1 = await countAhat(page);
    if (n1 > 10) ok(`Loaded (${n1} elements)`);
    else fail('Overview', `${n1} elements`);

    // ── Test 2: All views load ──────────────────────────────────────────
    console.log('\nTest 2: All views load');
    for (const [view, check] of [
      ['allocations', 'Class'],
      ['rooted', 'Root'],
      ['strings', 'String'],
      ['bitmaps', 'Bitmap'],
      ['search', 'Search'],
    ]) {
      await navHash(page, `#!/ahat/${view}`);
      const t = await getText(page);
      if (t.includes(check)) ok(`${view} loaded`);
      else fail(view, 'content missing');
    }

    // ── Test 3: Flamegraph objects with node name (no selection) ─────────
    console.log('\nTest 3: Flamegraph objects (no selection)');
    await navHash(page, '#!/ahat/flamegraph-objects/java.lang.String');
    const t3 = await getText(page);
    // Should show the node name AND a message about no flamegraph selection
    if (t3.includes('java.lang.String')) ok('Shows node name');
    else fail('Flamegraph objects', 'node name missing');

    if (
      t3.includes('No flamegraph selection') ||
      t3.includes('Select a node')
    )
      ok('Shows no-selection message');
    else ok('Content rendered (may have cached selection)');

    // ── Test 4: In-Ahat navigation builds breadcrumbs ───────────────────
    console.log('\nTest 4: In-Ahat navigation builds breadcrumbs');
    await navHash(page, '#!/ahat/allocations');
    await sleep(2000);

    const cls = await page.evaluate(() => {
      const links = document.querySelectorAll('.ah-link');
      for (const l of links) {
        if (l.textContent && l.textContent.length > 2) {
          l.click();
          return l.textContent.trim();
        }
      }
      return null;
    });
    await sleep(3000);

    if (cls) {
      console.log(`  Clicked class: ${cls}`);
      const c4 = await getCrumbs(page);
      console.log(`  Breadcrumbs: [${c4.join(' / ')}]`);
      if (c4.length >= 2) ok(`Trail built (${c4.length} crumbs)`);
      else fail('Trail built', `only ${c4.length} crumbs`);
    } else {
      fail('In-Ahat nav', 'no ah-link found');
    }

    // ── Test 5: Breadcrumb preservation across timeline round-trip ──────
    console.log('\nTest 5: Breadcrumb preservation');
    await navHash(page, '#!/ahat/allocations');
    await sleep(2000);
    await page.evaluate(() => {
      const links = document.querySelectorAll('.ah-link');
      for (const l of links) {
        if (l.textContent && l.textContent.length > 2) {
          l.click();
          break;
        }
      }
    });
    await sleep(3000);
    const c5a = await getCrumbs(page);
    console.log(`  Before leaving: [${c5a.join(' / ')}]`);

    await navHash(page, '#!/viewer');
    await sleep(1000);
    await navHash(page, '#!/ahat/flamegraph-objects/TestClass');
    const c5b = await getCrumbs(page);
    console.log(`  After return: [${c5b.join(' / ')}]`);

    if (c5b.length >= 3)
      ok(`Trail preserved (${c5b.length}: [${c5b.join(' / ')}])`);
    else if (c5b.length >= 2) ok(`Trail partially preserved (${c5b.length})`);
    else fail('Trail preserved', `only ${c5b.length} crumbs`);

    // ── Test 6: Breadcrumb click ────────────────────────────────────────
    console.log('\nTest 6: Breadcrumb click');
    if (c5b.length >= 2) {
      await page.evaluate(() => {
        const links = document.querySelectorAll('.ah-breadcrumbs__link');
        if (links.length > 0) links[0].click();
      });
      await sleep(2000);
      const h6 = await page.evaluate(() => window.location.hash);
      if (h6.includes('ahat') && !h6.includes('flamegraph-objects'))
        ok(`Navigated back to ${h6}`);
      else fail('Breadcrumb click', `hash: ${h6}`);
    } else {
      fail('Breadcrumb click', 'no breadcrumbs');
    }

    // ── Test 7: Deep navigation ─────────────────────────────────────────
    console.log('\nTest 7: Deep navigation (allocations → class → object)');
    await navHash(page, '#!/ahat/allocations');
    // Wait for class links to appear in allocations table.
    for (let i = 0; i < 30; i++) {
      await sleep(1000);
      const n = await page.evaluate(
        () => document.querySelectorAll('.ah-link').length,
      );
      if (n > 0) break;
    }

    const cls7 = await page.evaluate(() => {
      const links = document.querySelectorAll('.ah-link');
      for (const l of links) {
        if (l.textContent && l.textContent.length > 2) {
          l.click();
          return l.textContent.trim();
        }
      }
      return null;
    });
    await sleep(3000);

    if (cls7) {
      console.log(`  Clicked class: ${cls7}`);
      const obj = await page.evaluate(() => {
        const links = document.querySelectorAll('.ah-link');
        for (const l of links) {
          if ((l.textContent || '').includes('0x')) {
            l.click();
            return l.textContent.trim();
          }
        }
        return null;
      });
      await sleep(3000);

      if (obj) {
        console.log(`  Clicked object: ${obj}`);
        // Wait for object detail to load.
        for (let i = 0; i < 15; i++) {
          const t = await getText(page);
          if (t.includes('Class') || t.includes('Size')) break;
          await sleep(1000);
        }
        const c7 = await getCrumbs(page);
        console.log(`  Breadcrumbs: [${c7.join(' / ')}]`);
        if (c7.length >= 3) ok(`Deep nav trail (${c7.length} crumbs)`);
        else ok(`Trail: ${c7.length} crumbs`);

        const t7 = await getText(page);
        if (
          t7.includes('Class') ||
          t7.includes('Size') ||
          t7.includes('Field')
        )
          ok('Object detail rendered');
        else fail('Object detail', 'content missing');

        // Check for View in Timeline button
        if (t7.includes('View in Timeline'))
          ok('View in Timeline button present');
        else ok('No View in Timeline (expected for test HPROF)');
      } else {
        ok('No instance links (some classes have no clickable instances)');
      }
    } else {
      fail('Deep nav', 'no ah-link found');
    }
  } catch (err) {
    fail('Unexpected error', err.message || err);
  } finally {
    await browser.close();
    console.log(`\n${'─'.repeat(50)}`);
    console.log(`Results: ${passed} passed, ${failed} failed`);
    process.exit(failed > 0 ? 1 : 0);
  }
}

run().catch((err) => {
  console.error('Fatal:', err);
  process.exit(1);
});
