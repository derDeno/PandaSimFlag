// Run with node tests/check_info_webui.cjs; requires Playwright and Chrome.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {chromium} = require('playwright');

(async () => {
  const browser = await chromium.launch({channel: 'chrome', headless: true});
  try {
    const page = await browser.newPage({viewport: {width: 1200, height: 1000}});
    page.setDefaultTimeout(8000);
    const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
    const errors = [];
    const logRequests = [];
    let infoRequests = 0, matrixRequests = 0, failInfo = false, failLogs = false;
    let logText = '\x1b[32mI (100) PandaSimFlag: <device & log>\x1b[0m\n';
    let boot = '123', cursor = '50', dropped = '0';
    const state = {mode: 'station', connected: true, ip: '192.168.1.20', ssid: 'Racing network',
      hostname: 'pandasimflag.local', networkName: 'PandaSimFlag-1234', mac: '00:11:22:33:44:55',
      connectionMode: 'Wi-Fi', game: 'F1 2025', inputMode: 'wifi', wifiGame: 'f1-2025', displayType: 'flag',
      rssi: -50, version: '0.1.9', filesystemVersion: '0.1.9', usbCurrentMode: '1.5A', ledLimitMa: 800,
      ledPowerPercent: 25, matrixRotation: 0, uptime: 3661, gameConnected: true, gameSyncPaused: false,
      flag: 'green', animation: 'none', period: 250, phase: 0, remaining: 250, values: []};
    const info = {target: 'esp32s3', pcb_revision: '1.0', cpu_cores: 2, cpu_mhz: 240,
      flash_bytes: 8388608, reset_reason: 1, heap_free_bytes: 196608, heap_min_free_bytes: 131072,
      control_core: 0, matrix_core: 1, matrix_frames: 4321, matrix_stack_free_bytes: 8192};
    page.on('pageerror', e => errors.push(e.message));
    await page.route('https://api.github.com/**', route => route.fulfill({json: {
      tag_name: 'v0.2.0', draft: false, prerelease: false,
      assets: ['PandaSimFlag-firmware-v0.2.0-ota.bin', 'PandaSimFlag-filesystem-v0.3.0-ota.bin',
        'PandaSimFlag-v9.0.0-full.bin'].map(name => ({name, state: 'uploaded', size: 1024}))
    }}));
    await page.route('http://pandasimflag.test/**', async route => {
      const url = new URL(route.request().url());
      if (url.pathname === '/') return route.fulfill({contentType: 'text/html', body: html});
      if (url.pathname === '/api/state') return route.fulfill({json: state});
      if (url.pathname === '/api/matrix') { matrixRequests++; return route.fulfill({json: state}); }
      if (url.pathname === '/api/info') {
        infoRequests++;
        return route.fulfill(failInfo ? {status: 503} : {json: info});
      }
      if (url.pathname === '/api/logs') {
        logRequests.push(url.search);
        return route.fulfill(failLogs ? {status: 503} : {contentType: 'text/plain; charset=utf-8',
          headers: {'X-Log-Cursor': cursor, 'X-Log-Boot': boot, 'X-Log-Dropped': dropped}, body: logText});
      }
      return route.fulfill({status: 404});
    });
    const waitText = (selector, text) => page.waitForFunction(({selector, text}) =>
      document.querySelector(selector).textContent.includes(text), {selector, text});
    await page.goto('http://pandasimflag.test/');
    await waitText('#game', 'F1 2025');
    assert.equal(infoRequests, 0);
    assert.equal(logRequests.length, 0, 'Home does not fetch diagnostics or logs');
    await page.waitForSelector('#updates-toast:not([hidden])');
    await page.click('#updates-toast-close');
    await page.screenshot({path: path.join(os.tmpdir(), 'pandasimflag-latest-home.png'), fullPage: true});
    await page.click('[data-page="settings"]');
    await page.locator('#device-card summary').click();
    await page.fill('#device-hostname', 'unsaved-host');
    await page.click('[data-page="info"]');
    await waitText('#info-target', 'esp32s3');
    assert.equal(await page.locator('#info-flash').textContent(), '8.0 MiB');
    assert.equal(await page.locator('#info-cpu').textContent(), '240 MHz');
    assert.equal(await page.locator('#info-reset').textContent(), 'Power on');
    assert.equal(await page.locator('#info-matrix-core').textContent(), '1');
    assert.equal(await page.locator('#info-matrix-stack').textContent(), '8.0 KiB');
    assert.equal(await page.locator('#uptime').textContent(), '1h 1m 1s');
    assert.equal(await page.locator('#signal').textContent(), 'Excellent (-50 dBm)');
    assert.equal(await page.locator('#device-hostname').inputValue(), 'unsaved-host');
    assert.equal(logRequests.length, 0, 'Logs remain collapsed until requested');
    assert.equal(await page.locator('#firmware-update').isVisible(), true);
    assert.match(await page.locator('#filesystem-update').getAttribute('title'), /v0.3.0/);
    state.ip = '192.168.1.30'; info.heap_free_bytes = 163840;
    await page.click('#info-refresh');
    await waitText('#ip', '192.168.1.30');
    assert.equal(await page.locator('#info-heap-free').textContent(), '160.0 KiB');
    assert.equal(await page.locator('#updates-toast').isHidden(), true);
    assert.equal(await page.locator('#device-hostname').inputValue(), 'unsaved-host');
    failInfo = true;
    await page.click('#info-refresh');
    await waitText('#info-status', 'unavailable');
    assert.equal(await page.locator('#info-target').textContent(), 'esp32s3', 'Failure preserves the last snapshot');
    failInfo = false;
    await page.click('#info-refresh');
    await page.waitForFunction(() => document.querySelector('#info-status').hidden);
    const matrixCount = matrixRequests;
    await page.locator('#logs-details summary').click();
    await waitText('#logs-status-label', 'Live');
    assert.equal(await page.locator('#logs-output').textContent(), 'I (100) PandaSimFlag: <device & log>\n');
    assert.equal(await page.locator('#logs-output device').count(), 0, 'Device text must never become HTML');
    assert.equal(logRequests[0], '');
    await page.click('#logs-pause');
    await waitText('#logs-status-label', 'Paused');
    const pausedCount = logRequests.length;
    await page.waitForTimeout(2150);
    assert.equal(logRequests.length, pausedCount);
    assert.equal(matrixRequests, matrixCount, 'Info never polls the LED matrix');
    dropped = '1'; cursor = '60'; logText = 'next\n';
    await page.click('#logs-pause');
    await waitText('#logs-output', 'next');
    assert.equal(logRequests.at(-1), '?since=50&boot=123');
    assert.match(await page.locator('#logs-output').textContent(), /Earlier log output was overwritten/);
    await page.click('#logs-pause');
    dropped = '0'; boot = '456'; cursor = '9'; logText = 'new boot\n';
    await page.click('#logs-pause');
    await waitText('#logs-output', 'new boot');
    assert.equal(await page.locator('#logs-output').textContent(), '[Device restarted; previous logs cleared.]\nnew boot\n');
    await page.click('#logs-pause');
    failLogs = true;
    await page.click('#logs-pause');
    await waitText('#logs-status-label', 'Retrying');
    assert.equal(await page.locator('#logs-status').getAttribute('data-live'), 'false');
    assert.match(await page.locator('#logs-output').textContent(), /new boot/);
    await page.click('#logs-pause');
    failLogs = false; logText = 'x'.repeat(40000); cursor = '40009';
    await page.click('#logs-pause');
    await page.waitForFunction(() => document.querySelector('#logs-output').textContent.length === 32768);
    await page.click('#logs-pause');
    const downloadPromise = page.waitForEvent('download');
    await page.click('#logs-download');
    const download = await downloadPromise;
    assert.equal(download.suggestedFilename(), 'pandasimflag-logs.txt');
    assert.equal(fs.readFileSync(await download.path(), 'utf8'), await page.locator('#logs-output').textContent());
    await page.screenshot({path: path.join(os.tmpdir(), 'pandasimflag-latest-info.png'), fullPage: true});
    for (const width of [320, 390, 680, 681]) {
      await page.setViewportSize({width, height: 900});
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, `Info overflow at ${width}`);
      if (width === 390) await page.screenshot({path: path.join(os.tmpdir(), 'pandasimflag-latest-info-mobile.png'), fullPage: true});
    }
    await page.setViewportSize({width: 1200, height: 1000});
    logText = 'visible again\n';
    await page.click('#logs-pause');
    await waitText('#logs-status-label', 'Live');
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', {configurable: true, value: true});
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await waitText('#logs-status-label', 'Inactive');
    const hiddenCount = logRequests.length;
    await page.waitForTimeout(2150);
    assert.equal(logRequests.length, hiddenCount);
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', {configurable: true, value: false});
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await waitText('#logs-status-label', 'Live');
    await page.locator('#logs-details summary').click();
    await waitText('#logs-status-label', 'Inactive');
    const collapsedCount = logRequests.length;
    await page.waitForTimeout(2150);
    assert.equal(logRequests.length, collapsedCount);
    await page.evaluate(() => { location.hash = '#logs'; });
    await waitText('#logs-status-label', 'Live');
    assert.equal(await page.locator('#logs-details').getAttribute('open'), '');
    await page.click('[data-page="home"]');
    await waitText('#logs-status-label', 'Inactive');
    assert.equal(await page.locator('#info-page').isHidden(), true);
    assert.deepEqual(errors, []);
    console.log('Info/logs browser checks passed: snapshots, diagnostics, pending edits, update badges, bounded text, cursors, pause/resume, restart, errors, downloads, responsive layout and visibility/navigation polling.');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
