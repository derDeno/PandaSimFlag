// Run with node tests/check_updates.js; exercise the web UI's actual update checker.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const api = 'https://api.github.com/repos/derDeno/PandaSimFlag/releases/latest';
const releaseUrl = 'https://github.com/derDeno/PandaSimFlag/releases/tag/v0.2.0';
const asset = (kind, version) => ({name: `PandaSimFlag-${kind}-v${version}-ota.bin`, state: 'uploaded', size: 1024});
const release = {tag_name: 'v0.2.0', draft: false, prerelease: false,
  html_url: 'javascript:alert(1)', assets: [asset('firmware', '0.2.0'), asset('filesystem', '0.4.0')]};

function page(payload = release) {
  const elements = new Map([...html.matchAll(/id="([^"]+)"/g)].map(([, id]) => [`#${id}`, {
    hidden: true, textContent: '', setAttribute(name, value) { this[name] = value; }
  }]));
  const element = selector => {
    assert(elements.has(selector), `Missing ${selector}`);
    return elements.get(selector);
  };
  const timers = new Map();
  let timerId = 0;
  const requests = [];
  const context = vm.createContext({
    AbortController, document: {querySelector: element, querySelectorAll: () => []},
    location: {hostname: 'pandasimflag.local'},
    setTimeout(callback, delay) { timers.set(++timerId, {callback, delay}); return timerId; },
    clearTimeout(id) { timers.delete(id); },
    fetch: async (url, options) => { requests.push({url, options}); return {ok: true, json: async () => payload}; }
  });
  vm.runInContext("const $ = selector => document.querySelector(selector); const setText = (selector, value) => { $(selector).textContent = value; };" + script.slice(script.indexOf('    let installedVersions ='), script.indexOf('    function showDeviceStatus')), context);
  const run = code => vm.runInContext(code, context);
  const state = (firmware = '0.1.0', filesystem = '0.1.0') => {
    context.versions = {firmware, filesystem};
    run('installedVersions = versions; showAvailableUpdates()');
  };
  return {context, run, state, element, timers, requests};
}

(async () => {
  const versions = page();
  for (const [candidate, installed, newer] of [
    ['1.10.0', '1.9.9', true], ['v2.0.0', '1.99.99', true], ['0.1.1', '0.1.0', true],
    ['0.1.0', 'v0.1.0', false], ['0.1.0+new', '0.1.0+old', false], ['0.1.0', '0.2.0', false],
    ['0.1.0', '0.1.0-rc.1', true], ['0.2.0-beta.1', '0.1.0', false],
    ['0.2.0', 'unknown', false], ['vNext', '0.1.0', false], ['01.2.0', '0.1.0', false],
    ['0.2.0', '0.2.0-01', false], ['999999999999999999999.0.0', '0.1.0', false],
    [null, '0.1.0', false], ['0.2.0', undefined, false]
  ]) {
    versions.context.candidate = candidate;
    versions.context.installed = installed;
    assert.equal(versions.run('isNewerVersion(candidate, installed)'), newer, `${candidate} vs ${installed}`);
  }

  const updates = page();
  updates.state();
  updates.element('#settings-toast').hidden = false;
  updates.element('#toast-message').textContent = 'Existing device error';
  await updates.run('checkForUpdates()');
  await updates.run('checkForUpdates()');
  assert.equal(updates.requests.length, 1, 'Check GitHub only once per page load');
  assert.equal(updates.requests[0].url, api);
  assert.equal(updates.requests[0].options.credentials, 'omit');
  assert.equal(updates.requests[0].options.referrerPolicy, 'no-referrer');
  assert.equal(updates.timers.size, 0, 'No timeout or periodic timer remains after the check');
  for (const kind of ['firmware', 'filesystem']) {
    assert.equal(updates.element(`#${kind}-update`).hidden, false);
    assert.equal(updates.element(`#${kind}-update`).href, releaseUrl, 'Links stay on the trusted release page');
  }
  assert.match(updates.element('#firmware-update').title, /v0\.2\.0/);
  assert.match(updates.element('#filesystem-update').title, /v0\.4\.0/, 'Filesystem uses its own artifact version');
  assert.equal(updates.element('#updates-toast').hidden, false);
  assert.equal(updates.element('#updates-toast-link').href, releaseUrl);
  assert.equal(updates.element('#updates-toast-message').textContent, 'Update available: firmware v0.2.0 and filesystem v0.4.0.');
  assert.equal(updates.element('#settings-toast').hidden, false, 'Updates must not dismiss device errors');
  assert.equal(updates.element('#toast-message').textContent, 'Existing device error');
  updates.element('#updates-toast').hidden = true;
  updates.state('0.2.0', '0.3.0');
  assert.equal(updates.element('#firmware-update').hidden, true);
  assert.equal(updates.element('#filesystem-update').hidden, false);
  assert.equal(updates.element('#updates-toast').hidden, true, 'Refreshing Info must not reopen a dismissed update toast');
  assert.equal(updates.element('#updates-toast-message').textContent, 'Update available: filesystem v0.4.0.');
  updates.state('0.2.0', '0.4.0');
  assert.equal(updates.element('#filesystem-update').hidden, true);
  assert.equal(updates.element('#updates-toast').hidden, true);

  const earlyRelease = page();
  await earlyRelease.run('checkForUpdates()');
  assert.equal(earlyRelease.element('#updates-toast').hidden, true, 'Wait for installed versions before notifying');
  earlyRelease.state();
  assert.equal(earlyRelease.element('#updates-toast').hidden, false, 'Arrival order of device/GitHub responses must not matter');

  for (const payload of [null, {}, {...release, draft: true}, {...release, prerelease: true},
    {...release, tag_name: 'v0.3.0-rc.1'}, {...release, assets: []}, {...release, assets: [
      asset('full', '9.0.0'), asset('streamdeck', '9.0.0'), asset('firmware', 'invalid'),
      asset('filesystem', '0.5.0-beta.1'), {...asset('firmware', '0.2.0'), state: 'new'},
      {...asset('filesystem', '0.4.0'), size: 0}, null
    ]}]) {
    const unavailable = page(payload);
    unavailable.state();
    await unavailable.run('checkForUpdates()');
    assert.equal(unavailable.element('#firmware-update').hidden, true);
    assert.equal(unavailable.element('#filesystem-update').hidden, true);
    assert.equal(unavailable.element('#updates-toast').hidden, true);
  }
  const firmwareOnly = page({...release, assets: [asset('firmware', '0.3.0'), asset('firmware', '0.2.0')]});
  firmwareOnly.state();
  await firmwareOnly.run('checkForUpdates()');
  assert.match(firmwareOnly.element('#firmware-update').title, /v0\.3\.0/, 'Select the newest matching artifact');
  assert.equal(firmwareOnly.element('#filesystem-update').hidden, true, 'A firmware update must not imply a filesystem update');

  for (const fetch of [async () => ({ok: false, status: 403}), async () => ({ok: false, status: 404}),
    async () => { throw new Error('Offline'); }, async () => ({ok: true, json: async () => { throw new Error('Invalid JSON'); }})]) {
    const failed = page();
    failed.state();
    failed.context.fetch = fetch;
    await failed.run('checkForUpdates()');
    assert.equal(failed.element('#updates-toast').hidden, true);
    assert.equal(failed.element('#firmware-update').hidden, true);
    assert.equal(failed.timers.size, 0);
  }
  const timeout = page();
  timeout.state();
  timeout.context.fetch = (url, options) => new Promise((resolve, reject) => {
    options.signal.addEventListener('abort', () => reject(new Error('Timed out')));
  });
  const checking = timeout.run('checkForUpdates()');
  const timer = [...timeout.timers.values()][0];
  assert.equal(timer.delay, 8000);
  timer.callback();
  await checking;
  assert.equal(timeout.element('#updates-toast').hidden, true);
  assert.equal(timeout.timers.size, 0);
  console.log('Update checks passed: numeric versions, separate artifact versions, badges/toast, one request per load, response races, missing/partial assets, trusted links, offline/rate limits and timeouts.');
})().catch(error => { console.error(error); process.exitCode = 1; });
