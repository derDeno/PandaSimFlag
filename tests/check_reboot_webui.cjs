// Run with node tests/check_reboot_webui.cjs; requires Playwright and Chrome.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {chromium} = require('playwright');

async function main() {
  const browser = await chromium.launch({channel: 'chrome', headless: true});
  try {
    const page = await browser.newPage({viewport: {width: 375, height: 812}});
    const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
    const requests = [];
    const errors = [];
    let fail = true;
    let releaseReboot;
    page.on('pageerror', error => errors.push(error.message));
    await page.route('http://pandasimflag.test/**', async route => {
      const request = route.request();
      const endpoint = new URL(request.url()).pathname;
      if (endpoint === '/') return route.fulfill({contentType: 'text/html', body: html});
      if (endpoint === '/api/reboot') {
        requests.push([request.method(), request.postDataJSON()]);
        if (!fail) await new Promise(resolve => { releaseReboot = resolve; });
        return route.fulfill(fail
          ? {status: 500, json: {error: 'Reboot failed. Please retry.'}}
          : {json: {ok: true, restarting: true}});
      }
      assert.notEqual(endpoint, '/api/reset', 'Reboot must never erase settings');
      return route.fulfill({status: 503});
    });
    await page.goto('http://pandasimflag.test/#settings');
    const button = page.getByRole('button', {name: 'Reboot', exact: true});
    const dialog = page.locator('#update-dialog');
    const status = page.locator('#update-status');
    const waitStatus = text => page.waitForFunction(expected =>
      document.querySelector('#update-status').textContent.includes(expected), text);
    assert.equal(await button.isVisible(), true);
    assert.equal(await button.evaluate(el => el.parentElement.previousElementSibling.id), 'reset-card');
    assert.equal(await button.evaluate(el => el.closest('.card')), null);
    assert.equal(await page.locator('#reboot-message').count(), 0);
    assert.equal(await dialog.isVisible(), false);
    assert.equal(await button.evaluate(el => el.parentElement.scrollWidth <= el.parentElement.clientWidth), true);
    await button.click();
    await waitStatus('Please retry');
    assert.equal(await dialog.isVisible(), true);
    assert.equal(await status.evaluate(el => el.classList.contains('error')), true);
    assert.equal(await button.isEnabled(), true);
    await page.locator('#update-close').click();
    fail = false;
    await button.click();
    await waitStatus('Rebooting device…');
    assert.equal(await dialog.isVisible(), true);
    assert.equal(await page.locator('#update-title').textContent(), 'Rebooting device');
    assert.equal(await page.locator('#update-steps').isVisible(), false);
    assert.equal(await page.locator('#update-close').isVisible(), false);
    assert.equal(await status.evaluate(el => el.classList.contains('error')), false);
    await page.keyboard.press('Escape');
    assert.equal(await dialog.isVisible(), true);
    await button.dispatchEvent('click');
    assert.equal(requests.length, 2, 'An active reboot must not send another request');
    releaseReboot();
    await waitStatus('5 seconds');
    assert.equal(await button.isDisabled(), true);
    assert.equal(await dialog.evaluate(el => el.scrollWidth <= el.clientWidth), true);
    await page.screenshot({path: path.join(os.tmpdir(), 'pandasimflag-reboot-mobile.png')});
    await page.keyboard.press('Escape');
    assert.equal(await dialog.isVisible(), true);
    const started = Date.now();
    const reloaded = page.waitForEvent('domcontentloaded', {timeout: 7000});
    for (let seconds = 4; seconds >= 0; seconds--) {
      await waitStatus(`reload in ${seconds} ${seconds === 1 ? 'second.' : 'seconds.'}`);
    }
    await reloaded;
    assert.ok(Date.now() - started >= 4500, 'Reboot must allow time before reloading');
    assert.equal(new URL(page.url()).hash, '#settings');
    assert.equal(await button.isEnabled(), true);
    assert.deepEqual(requests, [['POST', {}], ['POST', {}]]);
    assert.deepEqual(errors, []);
    console.log('WebUI reboot: modal, mobile layout, POST, failure/retry, duplicate prevention, countdown from 5 to 0 and delayed reload passed.');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
