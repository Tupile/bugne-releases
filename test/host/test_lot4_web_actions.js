const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../components/web_config/www/index.html'), 'utf8');
const scripts = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/gi)].map(m => m[1]);
assert(scripts.length);
scripts.forEach(s => new vm.Script(s));
const markup = html.replace(/<script[^>]*>[\s\S]*?<\/script>/gi, '');
for (const match of markup.matchAll(/\bon(?:click|change|input|submit|timeupdate)=(?:"([^"]*)"|'([^']*)'|([^\s>]+))/g)) {
    new vm.Script('function handler(event){' + (match[1] || match[2] || match[3]) + '}');
}
function extract(name) {
    const start = html.indexOf('function ' + name + '(');
    assert(start >= 0);
    let end = html.indexOf('{', start), depth = 1;
    while (depth) {
        end++;
        if (html[end] === '{') depth++;
        if (html[end] === '}') depth--;
    }
    return html.slice(start, end + 1);
}
async function main() {
    let response, rejected = false, reboot = 0, message, calls = [];
    const elements = {ghinst: {disabled: false}, hatokensave: {disabled: false}, ha_token: {value: 'test-token'}};
    const cfg = {device: {name: 'unchanged'}};
    const context = vm.createContext({
        document: {getElementById: id => elements[id]}, cfg,
        confirm: () => true, tr: x => x, msg: x => {message = x;},
        ghShow: x => {message = x;}, fwReboot: () => {reboot++;},
        fetch: (url, options) => {
            calls.push({url, options});
            return rejected ? Promise.reject(new Error('offline')) : Promise.resolve(response);
        }
    });
    vm.runInContext(extract('ghInstall') + extract('changeHaToken'), context);
    for (const status of [400, 401, 500]) {
        response = {ok: false, status, text: () => Promise.resolve('server refused ' + status)};
        await context.ghInstall();
        assert.equal(reboot, 0);
        assert.equal(message, 'server refused ' + status);
        assert.equal(elements.ghinst.disabled, false);
    }
    rejected = true;
    await context.ghInstall();
    assert.equal(elements.ghinst.disabled, false);
    assert.equal(reboot, 0);
    rejected = false;
    response = {ok: true, text: () => Promise.resolve('update ok, rebooting')};
    await context.ghInstall();
    assert.equal(reboot, 1);
    assert.equal(elements.ghinst.disabled, true);
    const before = calls.length;
    await context.ghInstall();
    assert.equal(calls.length, before);
    response = {ok: false, text: () => Promise.resolve('token save failed')};
    await context.changeHaToken('test-token');
    assert.equal(message, 'token save failed');
    assert.equal(elements.ha_token.value, 'test-token');
    assert.equal(elements.hatokensave.disabled, false);
    response = {ok: true, text: () => Promise.resolve('token saved')};
    await context.changeHaToken('test-token');
    assert.equal(elements.ha_token.value, '');
    assert.equal(elements.hatokensave.disabled, false);
    await context.changeHaToken('');
    assert.equal(calls.at(-1).options.body, '');
    assert(calls.slice(before).every(c => c.url === '/api/ha/token'));
    assert.deepEqual(cfg, {device: {name: 'unchanged'}});
    assert(!extract('changeHaToken').includes('save()'));
    assert(html.includes('b_hatokensave:"Enregistrer le jeton"'));
    console.log('lot4 web: script/handler syntax, HTTP errors, retry, success-only reboot, token isolation passed');
}
main().catch(e => {console.error(e); process.exitCode = 1;});
