// Run with: node tests/check_webui.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(match => match[1]);
assert.equal(ids.length, new Set(ids).size, 'IDs must be unique');
for (const [, id] of script.matchAll(/\$\('#([^']+)'\)/g)) {
  assert(ids.includes(id), `Missing UI element: ${id}`);
}
for (const [, id] of html.matchAll(/\b(?:for|aria-labelledby)="([^"]+)"/g)) {
  assert(ids.includes(id), `Missing label target: ${id}`);
}

// Exercise the actual notification code with a clock we can advance.
const elements = new Map();
const timers = new Map();
let nextTimer = 0;
let interacting = false;
let animation;
function element(selector) {
  if (!elements.has(selector)) elements.set(selector, {
    hidden: true, textContent: '',
    matches: () => interacting,
    animate(keyframes, options) {
      animation = {
        currentTime: 0, playState: 'running',
        effect: {getTiming: () => options},
        pause() { this.playState = 'paused'; },
        play() { this.playState = 'running'; },
        cancel() { this.playState = 'idle'; }
      };
      return animation;
    }
  });
  return elements.get(selector);
}
const context = vm.createContext({
  $: element,
  setTimeout(callback, delay) { timers.set(++nextTimer, {callback, delay}); return nextTimer; },
  clearTimeout(id) { timers.delete(id); }
});
vm.runInContext(script.slice(script.indexOf('    let settingsToastTimer;'),
  script.indexOf("    $('#toast-close').addEventListener")), context);
const run = code => vm.runInContext(code, context);
run("showSettingsToast('Saved')");
assert.equal(element('#settings-toast').hidden, false);
assert.equal(element('#toast-countdown').hidden, false);
assert.equal(timers.get(nextTimer).delay, 5000);
assert.equal(animation.effect.getTiming().duration, 5000);
animation.currentTime = 1700;
interacting = true;
run('pauseSettingsToast(); resumeSettingsToast()');
assert.equal(timers.size, 0, 'Hover or focus pauses expiry');
assert.equal(animation.playState, 'paused');
interacting = false;
run('resumeSettingsToast()');
assert.equal(timers.get(nextTimer).delay, 3300, 'Resume preserves the remaining time');
const oldAnimation = animation;
run("showSettingsToast('Second save')");
assert.equal(oldAnimation.playState, 'idle', 'Replacing a toast cancels its animation');
assert.equal(timers.size, 1, 'Only the latest notification can expire');
assert.equal(element('#toast-message').textContent, 'Second save');
timers.get(nextTimer).callback();
assert.equal(element('#settings-toast').hidden, true);
assert.equal(element('#toast-message').textContent, '');
run("showSettingsToast('Request failed', 'error')");
assert.equal(element('#settings-toast').className, 'alert alert-error');
assert.equal(element('#settings-toast').hidden, false);
assert.equal(element('#toast-countdown').hidden, true);
assert.equal(timers.size, 0, 'Errors remain visible until dismissed');
run('clearSettingsToast(); resumeSettingsToast()');
assert.equal(element('#settings-toast').hidden, true);
assert.equal(timers.size, 0, 'Dismissal must prevent a paused toast from resuming');
interacting = true;
run("showSettingsToast('Saved while focused')");
assert.equal(animation.playState, 'paused');
assert.equal(timers.size, 0);
console.log('Web UI checks passed: script, element references, notification expiry, pause, replacement, errors and dismissal.');
