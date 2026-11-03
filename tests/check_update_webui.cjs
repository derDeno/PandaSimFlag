// Run with node tests/check_update_webui.cjs; requires Playwright and Chrome.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {chromium} = require('playwright');

async function main() {
  const browser = await chromium.launch({channel: 'chrome', headless: true});
  try {
    const page = await browser.newPage({viewport: {width: 1050, height: 900}});
    const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
    const errors = [];
    let navigations = 0;
    page.on('pageerror', error => errors.push(error.message));
    page.on('framenavigated', frame => { if (frame === page.mainFrame()) navigations++; });
    await page.route('http://pandasimflag.test/**', route => new URL(route.request().url()).pathname === '/'
      ? route.fulfill({contentType: 'text/html', body: html})
      : route.fulfill({status: 503}));
    await page.addInitScript(() => {
      window.updateRequests = [];
      window.XMLHttpRequest = class extends EventTarget {
        constructor() { super(); this.upload = new EventTarget(); }
        open(method, endpoint) { this.method = method; this.endpoint = endpoint; }
        send(data) { this.data = data; window.updateRequests.push(this); }
      };
    });
    const dialog = page.locator('#update-dialog');
    const status = page.locator('#update-status');
    const progress = page.locator('#update-progress');
    const installation = page.locator('#update-install-progress');
    const uploadStatus = page.locator('#update-upload-status');
    const installStatus = page.locator('#update-install-status');
    const waitStatus = text => page.waitForFunction(expected =>
      document.querySelector('#update-status').textContent.includes(expected), text);
    const startUpdate = async (kind, count = 1) => {
      await page.locator(kind === 'firmware' ? '#firmware-file' : '#filesystem-file').setInputFiles({
        name: `${kind}.bin`, mimeType: 'application/octet-stream', buffer: Buffer.from([0, 1, 2, 3])
      });
      await page.locator(kind === 'firmware' ? '#update-submit' : '#filesystem-update-submit').click();
      await page.waitForFunction(expected => window.updateRequests.length === expected, count);
      assert.equal(await dialog.isVisible(), true);
      assert.equal(await page.locator('#update-title').textContent(), `Updating ${kind}`);
      assert.equal(await uploadStatus.textContent(), '0%');
      assert.equal(await installStatus.textContent(), 'In progress...');
      assert.equal(await installation.getAttribute('value'), null);
      assert.equal(await status.textContent(), 'Uploading image. Keep the device powered.');
      assert.equal(await page.locator('#update-submit').isDisabled(), true);
      assert.equal(await page.locator('#filesystem-update-submit').isDisabled(), true);
      assert.deepEqual(await page.evaluate(() => {
        const request = window.updateRequests.at(-1);
        return [request.method, request.endpoint, request.data.get('update').name];
      }), ['POST', `/api/update/${kind}`, `${kind}.bin`]);
    };
    const finish = (status, responseText = '') => page.evaluate(({status, responseText}) => {
      const request = window.updateRequests.at(-1);
      Object.assign(request, {status, responseText});
      request.dispatchEvent(new Event('load'));
    }, {status, responseText});

    for (const kind of ['firmware', 'filesystem']) {
      await page.goto('http://pandasimflag.test/#settings');
      await page.locator('#update-card summary').click();
      await startUpdate(kind);
      await page.keyboard.press('Escape');
      assert.equal(await dialog.isVisible(), true, 'An active update must stay visible');
      await page.evaluate(() => uploadUpdate('#filesystem-file', '#filesystem-update-message', '/api/update/filesystem', 'Filesystem'));
      assert.equal(await page.evaluate(() => window.updateRequests.length), 1);

      await page.evaluate(() => window.updateRequests[0].upload.dispatchEvent(new ProgressEvent('progress')));
      assert.equal(await progress.getAttribute('value'), null, 'Unknown upload length is indeterminate');
      assert.equal(await uploadStatus.textContent(), 'Uploading…');
      await page.evaluate(() => window.updateRequests[0].upload.dispatchEvent(new ProgressEvent('progress', {
        lengthComputable: true, loaded: 8, total: 100
      })));
      assert.equal(await uploadStatus.textContent(), '8%');
      assert.equal(await progress.getAttribute('value'), '8');
      assert.equal(await installStatus.textContent(), 'In progress...');
      await page.screenshot({path: path.join(os.tmpdir(), `pandasimflag-${kind}-update-upload.png`)});
      await page.evaluate(() => window.updateRequests[0].upload.dispatchEvent(new Event('load')));
      await waitStatus('Finishing installation and verifying the image...');
      assert.equal(await progress.getAttribute('value'), '100');
      assert.equal(await uploadStatus.textContent(), '100%');
      assert.equal(await installation.getAttribute('value'), null, 'Installation waits for device confirmation');
      assert.equal(await installStatus.textContent(), 'Verifying...');
      assert.equal(await page.locator('#update-close').isVisible(), false);
      await page.screenshot({path: path.join(os.tmpdir(), `pandasimflag-${kind}-update.png`)});
      await page.setViewportSize({width: 375, height: 812});
      assert.equal(await dialog.evaluate(el => el.scrollWidth <= el.clientWidth), true);
      await page.screenshot({path: path.join(os.tmpdir(), `pandasimflag-${kind}-update-mobile.png`)});
      await page.setViewportSize({width: 1050, height: 900});

      const beforeSuccess = navigations;
      const completedAt = Date.now();
      await finish(200, '{"ok":true,"restarting":true}');
      await waitStatus('Update installed successfully.');
      assert.match(await status.textContent(), /5 seconds/);
      assert.equal(await progress.getAttribute('value'), '100');
      assert.equal(await installation.getAttribute('value'), '100');
      assert.equal(await installStatus.textContent(), 'Complete');
      await page.screenshot({path: path.join(os.tmpdir(), `pandasimflag-${kind}-update-complete.png`)});
      await page.setViewportSize({width: 375, height: 812});
      assert.equal(await dialog.evaluate(el => el.scrollWidth <= el.clientWidth), true);
      await page.screenshot({path: path.join(os.tmpdir(), `pandasimflag-${kind}-update-complete-mobile.png`)});
      await page.setViewportSize({width: 1050, height: 900});
      const reloaded = page.waitForEvent('domcontentloaded', {timeout: 7000});
      for (let seconds = 4; seconds >= 0; seconds--) {
        await waitStatus(`reload in ${seconds} ${seconds === 1 ? 'second.' : 'seconds.'}`);
        assert.equal(navigations, beforeSuccess, 'Must finish the countdown before reloading');
        assert.equal(await page.locator(kind === 'firmware' ? '#update-message' : '#filesystem-update-message').textContent(),
          await status.textContent());
      }
      await reloaded;
      assert.ok(Date.now() - completedAt >= 4900, 'Reload must wait five seconds after success');
      assert.equal(navigations, beforeSuccess + 1);
      assert.equal(await dialog.isVisible(), false);
    }

    await page.locator('#update-card summary').click();
    const beforeFailure = navigations;
    await startUpdate('firmware');
    await finish(400, '{"error":"Invalid firmware image"}');
    await waitStatus('Update failed: Invalid firmware image');
    assert.equal(await progress.isVisible(), false);
    assert.equal(await page.locator('#update-submit').isEnabled(), true);
    assert.equal(await page.locator('#filesystem-update-submit').isEnabled(), true);
    await page.keyboard.press('Escape');
    assert.equal(await dialog.isVisible(), false);

    await startUpdate('filesystem', 2);
    await finish(500, 'Could not write filesystem');
    await waitStatus('Update failed: Could not write filesystem');
    await page.locator('#update-close').click();
    for (const [event, message] of [['error', 'Connection lost'], ['abort', 'aborted']]) {
      await startUpdate('filesystem', event === 'error' ? 3 : 4);
      await page.evaluate(event => window.updateRequests.at(-1).dispatchEvent(new Event(event)), event);
      await waitStatus(message);
      await page.locator('#update-close').click();
    }
    await page.waitForTimeout(5100);
    assert.equal(navigations, beforeFailure, 'Failures must not reload the page');
    assert.deepEqual(errors, []);
    console.log('WebUI updates: both images, upload percentages, installation, completion, countdown from 5 to 0, delayed reload, mobile layout, failure and retry passed.');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
