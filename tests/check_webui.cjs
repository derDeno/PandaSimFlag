// Run with node tests/check_webui.cjs; requires Playwright and Chrome.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');

async function main() {
  const browser = await chromium.launch({channel: 'chrome', headless: true});
  try {
    const page = await browser.newPage({viewport: {width: 1050, height: 900}});
    const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
    const state = {
      mode: 'station', connected: true, ip: '192.168.1.20', ssid: 'Test network',
      hostname: 'pandasimflag.local', deviceHostname: 'PandaSimFlag-1D88', networkName: 'PandaSimFlag-1D88', mac: '00:11:22:33:44:55',
      connectionMode: 'Standby', game: 'Not reported', inputMode: 'wifi', wifiGame: '',
      displayType: 'flag', rssi: -50, version: 'test', filesystemVersion: 'test',
      powerController: 'TUSB320', usbCurrentMode: 'default current', ledLimitMa: 200,
      ledPowerPercent: 25, matrixRotation: 90, uptime: 60, normalFlagsBlink: false, normalFlagBlinkPeriod: 250,
      gameConnected: false, gameSyncPaused: false, outputBrightness: 63,
      flag: 'idle', animation: 'none', period: 250, phase: 0, remaining: 120,
      pixels: Array(192).fill(0)
    };
    const commands = [];
    const savedSettings = [];
    const errors = [];
    let offline = false;
    let rejectCommand = false;
    let stateRequests = 0;
    let matrixRequests = 0;
    const orientationCommands = [];
    let assistant = null;
    let nextSession = 1;
    let rejectOrientationAction = '';
    page.on('pageerror', error => errors.push(error.message));
    await page.route('http://pandasimflag.test/**', async route => {
      const url = new URL(route.request().url());
      if (url.pathname === '/') return route.fulfill({contentType: 'text/html', body: html});
      if (url.pathname === '/api/state') {
        stateRequests++;
        if (offline) return route.abort();
        return route.fulfill({json: state});
      }
      if (url.pathname === '/api/matrix') {
        matrixRequests++;
        if (offline) return route.abort();
        const {gameConnected, gameSyncPaused, orientationActive, flag, animation, period, phase, remaining, values} = state;
        return route.fulfill({json: {gameConnected, gameSyncPaused, orientationActive, flag, animation, period, phase, remaining, values,
          ...(flag === 'simhub-usb' ? {pixels: state.pixels} : {})}});
      }
      if (url.pathname === '/api/game') {
        commands.push(route.request().postDataJSON());
        return rejectCommand
          ? route.fulfill({status: 503, json: {error: 'Telemetry queue is full'}})
          : route.fulfill({status: 204});
      }
      if (url.pathname === '/api/settings') {
        const settings = route.request().postDataJSON();
        savedSettings.push(settings);
        if ('matrixRotation' in settings) state.matrixRotation = settings.matrixRotation;
        return route.fulfill({json: {ok: true}});
      }
      if (url.pathname === '/api/orientation') {
        const command = route.request().postDataJSON();
        orientationCommands.push(command);
        const {action, session, rotation} = command;
        if (action === rejectOrientationAction) return route.fulfill({status: 500, json: {error: 'Could not save orientation. Please retry.'}});
        if (action === 'start') {
          if (assistant) return route.fulfill({status: 409, json: {error: 'An orientation assistant is already open.'}});
          assistant = {session: nextSession++, rotation: state.matrixRotation ?? 0};
          state.orientationActive = true;
          state.gameSyncPaused = true;
          state.flag = 'orientation';
        } else {
          if (!assistant || assistant.session !== session) return route.fulfill({status: 409, json: {error: 'Orientation assistant expired. Close and reopen it.'}});
          if (action === 'preview') assistant.rotation = rotation;
          if (action === 'save' || action === 'cancel') {
            if (action === 'save') state.matrixRotation = assistant.rotation;
            assistant = null;
            state.orientationActive = false;
            state.gameSyncPaused = false;
            state.flag = 'idle';
          }
        }
        return route.fulfill({json: assistant || {session: 0, rotation: state.matrixRotation}});
      }
      return route.fulfill({status: 404});
    });
    const color = () => page.locator('.pixel').first().evaluate(el => el.style.backgroundColor);
    const waitColor = value => page.waitForFunction(expected =>
      document.querySelector('.pixel').style.backgroundColor === expected, value);
    const waitStatus = value => page.waitForFunction(expected =>
      document.querySelector('#matrix-status').textContent.includes(expected), value);

    await page.goto('http://pandasimflag.test/');
    await waitStatus('Matrix off');
    await waitColor('rgb(0, 0, 0)');
    assert.equal(await page.locator('#ip').textContent(), state.ip);
    assert.equal(await page.locator('#stop-preview-button').isDisabled(), true);

    // Prefill the connected network without using the device's setup hotspot name.
    assert.equal(await page.locator('#ssid').inputValue(), state.ssid);
    state.mode = 'ap';
    state.ssid = 'PandaSimFlag-1D88';
    await page.reload();
    await waitStatus('Matrix off');
    assert.equal(await page.locator('#ssid').inputValue(), '');
    state.mode = 'station';
    state.ssid = '';
    await page.reload();
    await waitStatus('Matrix off');
    assert.equal(await page.locator('#ssid').inputValue(), '');
    state.ssid = 'Test network';

    // Show the registered mDNS address, which can differ from the configured hostname.
    for (const hostname of ['pandasimflag.local', 'pandasimflag-1d88.local', 'my-flag.local']) {
      state.hostname = hostname;
      await page.reload();
      await waitStatus('Matrix off');
      assert.equal(await page.locator('#hostname-help').textContent(),
        `Edit the .local address. Network device name: ${state.networkName}.`);
      assert.equal(await page.locator('#device-hostname').inputValue(), hostname);
    }
    const hostnameField = page.locator('#device-hostname');
    for (const value of ['race-flag.local', 'race-flag', 'a.local', `${'a'.repeat(30)}.local`]) {
      await hostnameField.evaluate((el, name) => { el.value = name; }, value);
      assert.equal(await hostnameField.evaluate(el => el.checkValidity()), true, value);
    }
    for (const value of ['', '-flag.local', 'flag-.local', 'flag.example.com', `${'a'.repeat(31)}.local`]) {
      await hostnameField.evaluate((el, name) => { el.value = name; }, value);
      assert.equal(await hostnameField.evaluate(el => el.checkValidity()), false, value);
    }
    await hostnameField.evaluate((el, name) => { el.value = name; }, state.hostname);

    // Keep browser glyphs identical to the firmware, including reverse and neutral.
    const firmware = fs.readFileSync(path.join(__dirname, '../src/main.cpp'), 'utf8');
    const glyphBlock = firmware.match(/const char\* patterns\[\] = \{([\s\S]*?)\n  \};/)[1];
    const glyphs = glyphBlock.trim().split(/,\s*/).map(row => [...row.matchAll(/"([01]+)"/g)].map(match => match[1]).join(''));
    const names = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'N', 'R'];
    for (let i = 0; i < names.length; i++) {
      const rendered = await page.evaluate(flag => patternColors({flag, animation: 'none'}, 0)
        .map(color => color[0] ? '1' : '0').join(''), `gear-${names[i]}`);
      assert.equal(rendered, glyphs[i], `gear-${names[i]}`);
    }
    const patterns = await page.evaluate(() => {
      const pattern = (flag, animation = 'none', phase = 0, values = []) => patternColors({flag, animation, values}, phase);
      const lit = colors => colors.filter(color => color.some(Boolean)).length;
      return {
        idle: lit(pattern('idle')), green: pattern('green')[0], black: pattern('black')[0],
        checkered: lit(pattern('checkered')), diagonal: lit(pattern('blackwhite')),
        doubleyellow: lit(pattern('doubleyellow')), swapped: lit(pattern('doubleyellow', 'doubleyellow', 1)),
        blink: lit(pattern('red', 'blink', 1)), inverted: pattern('checkered', 'invert', 1)[0],
        rain: Array.from({length: 8}, (_, phase) => lit(pattern('rain', 'rain', phase))),
        rainTop: pattern('rain', 'rain', 0)[1], rainNext: pattern('rain', 'rain', 1)[9],
        pedals: lit(pattern('pedals', 'none', 0, [13, 21])),
        pedalsBottom: pattern('pedals', 'none', 0, [1, 1]).slice(56),
        rpm: [47, 48, 55, 56, 63].map(i => pattern('rpm', 'none', 0, [64])[i]),
        tyres: [0, 4, 32, 36].map(i => pattern('tyres', 'none', 0,
          [0, 0, 255, 0, 128, 0, 255, 0, 0, 0, 0, 0])[i]),
        proximity: ['left', 'right', 'both'].map(side => lit(pattern(`proximity-${side}`)))
      };
    });
    assert.deepEqual(patterns, {
      idle: 0, green: [0, 128, 0], black: [48, 0, 0], checkered: 32, diagonal: 36,
      doubleyellow: 28, swapped: 36, blink: 0, inverted: [0, 0, 0], rain: Array(8).fill(21),
      rainTop: [0, 150, 255], rainNext: [0, 150, 255], pedals: 34,
      pedalsBottom: [[0, 128, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0], [255, 0, 0], [0, 0, 0], [0, 0, 0], [0, 0, 0]],
      rpm: [[0, 128, 0], [255, 165, 0], [255, 165, 0], [255, 0, 0], [255, 0, 0]],
      tyres: [[0, 0, 255], [0, 128, 0], [255, 0, 0], [0, 0, 0]], proximity: [8, 8, 16]
    });

    // Orientation saves independently, so adjusting it cannot submit other pending settings.
    assert.equal(await page.locator('#matrix-rotation').textContent(), '90° clockwise');
    assert.equal(await page.locator('select#matrix-rotation').count(), 0);
    await page.goto('http://pandasimflag.test/#settings');
    await page.locator('#device-card summary').click();
    const dialog = page.locator('#orientation-dialog');
    const waitOrientation = value => page.waitForFunction(expected =>
      document.querySelector('#orientation-preview').textContent === expected &&
      !document.querySelector('#orientation-right').disabled, value);
    await page.locator('#led-power').fill('60');
    await page.locator('#orientation-adjust').click();
    await waitOrientation('90° clockwise');
    assert.equal(await dialog.isVisible(), true);
    assert.match(await page.locator('#orientation-description').textContent(), /red dot.*top-left/);
    assert.equal(state.orientationActive, true);
    assert.equal(state.gameSyncPaused, true);
    assert.equal(state.matrixRotation, 90);
    assert.equal(savedSettings.length, 0);
    const marker = await page.evaluate(() => patternColors({flag: 'orientation', animation: 'none'}, 0));
    assert.deepEqual(marker[0], [255, 0, 0]);
    assert.equal(marker.filter(color => color.some(Boolean)).length, 1);
    await page.locator('#orientation-right').click();
    await waitOrientation('180° clockwise');
    await page.locator('#orientation-right').click();
    await waitOrientation('270° clockwise');
    await page.locator('#orientation-right').click();
    await waitOrientation('0° (default)');
    await page.locator('#orientation-left').click();
    await waitOrientation('270° clockwise');
    assert.equal(state.matrixRotation, 90, 'Preview must not save');
    await page.waitForFunction(() => document.querySelector('#orientation-save').disabled === false);
    await page.waitForTimeout(3100);
    assert.ok(orientationCommands.some(command => command.action === 'keepalive'));
    await page.screenshot({path: path.join(process.env.TEMP, 'pandasimflag-orientation-desktop.png')});
    await page.setViewportSize({width: 375, height: 812});
    await page.screenshot({path: path.join(process.env.TEMP, 'pandasimflag-orientation-mobile.png')});
    assert.equal(await dialog.evaluate(el => el.scrollWidth <= el.clientWidth), true);
    await page.setViewportSize({width: 1050, height: 900});

    rejectOrientationAction = 'save';
    await page.locator('#orientation-save').click();
    await page.waitForFunction(() => document.querySelector('#orientation-message').textContent.includes('retry'));
    assert.equal(await dialog.isVisible(), true);
    assert.equal(state.matrixRotation, 90);
    rejectOrientationAction = '';
    await page.locator('#orientation-save').click();
    await dialog.waitFor({state: 'hidden'});
    assert.equal(state.matrixRotation, 270);
    assert.equal(state.gameSyncPaused, false);
    assert.equal(await page.locator('#matrix-rotation').textContent(), '270° clockwise');
    assert.equal(await page.locator('#led-power').inputValue(), '60');
    assert.equal(savedSettings.length, 0);
    assert.equal(await page.locator('#device-message').textContent(), '');
    assert.equal(await page.locator('#save-toast').textContent(), 'Matrix orientation saved.');
    await page.waitForFunction(() => document.querySelector('#save-toast').textContent === '');

    await page.locator('#orientation-adjust').click();
    await waitOrientation('270° clockwise');
    await page.locator('#orientation-left').click();
    await waitOrientation('180° clockwise');
    await page.locator('#orientation-cancel').click();
    await dialog.waitFor({state: 'hidden'});
    assert.equal(state.matrixRotation, 270);
    assert.equal(state.gameSyncPaused, false);
    await page.locator('#orientation-adjust').click();
    await waitOrientation('270° clockwise');
    await page.keyboard.press('Escape');
    await dialog.waitFor({state: 'hidden'});
    assert.equal(orientationCommands.at(-1).action, 'cancel');
    assert.equal(state.gameSyncPaused, false);

    // An expired session disables preview/save and can still be closed.
    await page.locator('#orientation-adjust').click();
    await waitOrientation('270° clockwise');
    assistant = null;
    state.orientationActive = false;
    state.gameSyncPaused = false;
    state.flag = 'idle';
    await page.locator('#orientation-right').click();
    await page.waitForFunction(() => document.querySelector('#orientation-message').textContent.includes('expired'));
    assert.equal(await page.locator('#orientation-save').isDisabled(), true);
    await page.locator('#orientation-cancel').click();
    await dialog.waitFor({state: 'hidden'});

    // An assistant already owned by another tab cannot be taken over.
    assistant = {session: nextSession++, rotation: state.matrixRotation};
    await page.locator('#orientation-adjust').click();
    await page.waitForFunction(() => document.querySelector('#orientation-message').textContent.includes('already open'));
    assert.equal(await page.locator('#orientation-right').isDisabled(), true);
    const beforeClose = orientationCommands.length;
    await page.locator('#orientation-cancel').click();
    await dialog.waitFor({state: 'hidden'});
    assert.equal(orientationCommands.length, beforeClose, 'Must not cancel another tab’s session');
    assistant = null;

    await page.locator('#device-submit').click();
    await page.waitForFunction(() => document.querySelector('#save-toast').textContent.includes('saved'));
    assert.equal(savedSettings.at(-1).hostname, 'my-flag');
    assert.equal('matrixRotation' in savedSettings.at(-1), false);
    await page.reload();
    assert.equal(await page.locator('#matrix-rotation').textContent(), '270° clockwise');
    delete state.matrixRotation;
    await page.reload();
    assert.equal(await page.locator('#matrix-rotation').textContent(), '0° (default)');
    await page.goto('http://pandasimflag.test/');
    await waitStatus('Matrix off');

    // Home reflects an assistant opened elsewhere and prevents conflicting demos.
    state.orientationActive = true;
    state.gameSyncPaused = true;
    state.flag = 'orientation';
    await waitStatus('Orientation assistant active');
    await waitColor('rgb(255, 0, 0)');
    assert.equal(await page.locator('#preview-button').isDisabled(), true);
    assert.equal(await page.locator('#stop-preview-button').isDisabled(), true);
    state.orientationActive = false;
    state.gameSyncPaused = false;
    state.flag = 'idle';
    await waitStatus('Matrix off');

    // Only a device-reported descriptor may start the local preview.
    await page.locator('#preview-button').click();
    assert.equal(commands.at(-1).display, 'green');
    assert.equal(await color(), 'rgb(0, 0, 0)');
    state.gameSyncPaused = true;
    state.flag = 'green';
    state.pixels = Array(64).fill([0, 128, 0]).flat();
    await waitColor('rgb(0, 128, 0)');
    await waitStatus('Game sync paused');
    assert.equal(await page.locator('#stop-preview-button').isEnabled(), true);

    // Selecting another pattern keeps control until the explicit Stop command.
    const beforeSelection = commands.length;
    await page.locator('#display-select').selectOption('rain');
    assert.equal(commands.length, beforeSelection);
    await page.locator('#preview-button').click();
    assert.equal(commands.at(-1).animation, 'rain');
    assert.equal(await color(), 'rgb(0, 128, 0)');
    state.flag = 'idle';
    await waitColor('rgb(0, 0, 0)');
    state.flag = 'simhub-usb';
    state.pixels = Array(64).fill([0, 150, 255]).flat();
    await waitColor('rgb(0, 150, 255)');
    // Hardware brightness must never dim or hide the frame in the Web UI.
    state.ledPowerPercent = 0;
    state.outputBrightness = 0;
    await page.waitForTimeout(300);
    assert.equal(await page.locator('#led-power-value').evaluate(el => el.value), '25%');
    assert.equal(await color(), 'rgb(0, 150, 255)');
    state.ledPowerPercent = 100;
    state.outputBrightness = 255;
    assert.equal(await color(), 'rgb(0, 150, 255)');
    delete state.outputBrightness;
    state.ledPowerPercent = 1;
    assert.equal(await color(), 'rgb(0, 150, 255)');
    assert.equal(await page.locator('#preview-button').isEnabled(), true);

    // Device status is fetched once per load, and fields change only on reload.
    const fullRequests = stateRequests;
    state.game = 'Changed game';
    state.rssi = -80;
    await page.waitForTimeout(350);
    assert.equal(stateRequests, fullRequests);
    assert.equal(await page.locator('#game').textContent(), 'Not reported');
    assert.equal(await page.locator('#signal').textContent(), '-50 dBm');

    // Built-in animations continue between matrix responses, without RGB payloads.
    state.flag = 'green';
    state.animation = 'blink';
    state.phase = 1;
    state.period = 100;
    await waitColor('rgb(0, 0, 0)');
    await waitColor('rgb(0, 128, 0)');
    state.animation = 'none';
    state.phase = 0;
    await waitColor('rgb(0, 128, 0)');

    // Settings and hidden tabs do not poll or keep local animation timers alive.
    await page.locator('[data-page="settings"]').click();
    await page.waitForTimeout(150);
    const settingsRequests = matrixRequests;
    await page.waitForTimeout(400);
    assert.equal(matrixRequests, settingsRequests);
    await page.locator('[data-page="home"]').click();
    await page.waitForFunction(() => document.querySelector('#matrix-status').textContent.includes('Game sync paused'));
    await page.waitForTimeout(300);
    assert.ok(matrixRequests > settingsRequests);
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', {configurable: true, value: true});
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForTimeout(150);
    const hiddenRequests = matrixRequests;
    await page.waitForTimeout(400);
    assert.equal(matrixRequests, hiddenRequests);
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', {configurable: true, value: false});
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForTimeout(300);
    assert.ok(matrixRequests > hiddenRequests);

    // Pause status and Stop survive reloads and failed release requests.
    await page.reload();
    await waitStatus('Game sync paused');
    assert.equal(await page.locator('#game').textContent(), 'Changed game');
    assert.equal(await page.locator('#signal').textContent(), '-80 dBm');
    rejectCommand = true;
    await page.locator('#stop-preview-button').click();
    await page.waitForFunction(() => document.querySelector('#home-message').textContent.includes('failed'));
    await waitStatus('Game sync paused');
    assert.equal(await page.locator('#stop-preview-button').isEnabled(), true);
    rejectCommand = false;
    await page.locator('#stop-preview-button').click();
    assert.equal(commands.at(-1).display, 'idle');
    state.gameSyncPaused = false;
    state.flag = 'idle';
    state.pixels.fill(0);
    await waitStatus('Matrix off');
    state.gameConnected = true;
    state.flag = 'simhub-usb';
    state.pixels = Array(64).fill([255, 0, 0]).flat();
    await waitColor('rgb(255, 0, 0)');
    await waitStatus('Game sync active');

    offline = true;
    await waitStatus('Live view is stale');
    assert.equal(await page.locator('#preview-button').isDisabled(), true);
    assert.equal(await color(), 'rgb(255, 0, 0)');
    offline = false;
    delete state.gameSyncPaused;
    await waitStatus('Update firmware');
    assert.deepEqual(errors, []);
    console.log('WebUI: live orientation assistant, independent save/cancel, Escape, failure/retry, session conflict/expiry, heartbeat, toast, responsive dialog, manual controls, animations and offline recovery passed.');
  } finally {
    await browser.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
