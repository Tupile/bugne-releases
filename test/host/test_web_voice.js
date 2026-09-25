// Parent voice message (Play tab): WAV encoding, resampling, status messages
// and the vmSend pipeline against scripted browser audio APIs.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../components/web_config/www/index.html'), 'utf8');
function extract(name) {
    const start = html.indexOf('function ' + name + '(');
    assert(start >= 0, name + ' not found');
    let end = html.indexOf('{', start), depth = 1;
    while (depth) {
        end++;
        if (html[end] === '{') depth++;
        if (html[end] === '}') depth--;
    }
    return html.slice(start, end + 1);
}
const code = ['vmWav', 'vmResample', 'vmErr', 'vmSend'].map(extract).join('\n');
assert(/id=vmfile[^>]*accept="audio\/\*"/.test(html), 'file input');
assert(!/id=vmfile[^>]*capture/.test(html), 'capture opens nothing on Android Chrome');

function run(env) {
    const ctx = vm.createContext(Object.assign({Math, Promise, Float32Array, Int16Array,
        ArrayBuffer, DataView, encodeURIComponent, Blob: class { constructor(p) { this.parts = p; } },
        tr: x => x}, env));
    vm.runInContext('var VM_MAX_S=60;var VM_RATE=16000;' + code, ctx);
    return ctx;
}

async function main() {
    // WAV: 44-byte PCM header, 16-bit mono, clamped samples.
    const c = run({});
    const w = new DataView(c.vmWav(new Float32Array([0, 1, -1, 2, -0.5]), 16000));
    const tag = o => String.fromCharCode(...[0, 1, 2, 3].map(i => w.getUint8(o + i)));
    assert.equal(w.byteLength, 44 + 10);
    assert.equal(tag(0), 'RIFF'); assert.equal(tag(8), 'WAVE'); assert.equal(tag(12), 'fmt ');
    assert.equal(tag(36), 'data');
    assert.equal(w.getUint32(4, true), 36 + 10);
    assert.equal(w.getUint16(20, true), 1);      // PCM
    assert.equal(w.getUint16(22, true), 1);      // mono
    assert.equal(w.getUint32(24, true), 16000);
    assert.equal(w.getUint32(28, true), 32000);  // byte rate
    assert.equal(w.getUint16(32, true), 2);
    assert.equal(w.getUint16(34, true), 16);
    assert.equal(w.getUint32(40, true), 10);
    assert.deepEqual([0, 1, 2, 3, 4].map(i => w.getInt16(44 + i * 2, true)),
                     [0, 32767, -32768, 32767, -16384]);

    // Resample: length follows the ratio, a constant stays constant.
    const r = c.vmResample(new Float32Array(48000).fill(0.25), 48000, 16000);
    assert.equal(r.length, 16000);
    assert(r.every(x => Math.abs(x - 0.25) < 1e-6));
    assert.equal(c.vmResample(new Float32Array(441), 44100, 16000).length, 160);

    // Status messages: one per server answer, a generic one otherwise.
    for (const s of [403, 503, 507, 413, 400]) assert.notEqual(c.vmErr(s), c.vmErr(500));
    assert.equal(c.vmErr(418), c.vmErr(500));

    // vmSend pipeline.
    async function send({duration = 2, rate = 44100, decodeFails = false, offlineThrows = false,
                         status = 200, netFails = false, from = 'Parent'}) {
        const msgs = [], posts = [];
        let closed = 0;
        const buf = {duration, sampleRate: rate, numberOfChannels: 2,
                     getChannelData: () => new Float32Array(Math.round(duration * rate)).fill(0.1)};
        class AC {
            decodeAudioData(ab, ok, ko) { decodeFails ? ko(new Error('bad')) : ok(buf); }
            close() { closed++; }
        }
        class OAC {
            constructor(ch, n, sr) {
                if (offlineThrows) throw new Error('rate');
                assert.equal(ch, 1); assert.equal(sr, 16000); this.n = n;
            }
            get destination() { return {}; }
            createBufferSource() { return {connect() {}, start() {}}; }
            startRendering() {
                const n = this.n;
                return Promise.resolve({getChannelData: () => new Float32Array(n).fill(0.1)});
            }
        }
        const input = {files: [{arrayBuffer: () => Promise.resolve(new ArrayBuffer(8))}], value: 'x'};
        const ctx = run({
            window: {AudioContext: AC, OfflineAudioContext: OAC},
            document: {getElementById: id => ({vmfrom: {value: from}})[id]},
            msg: m => msgs.push(m),
            fetch: (url, opt) => {
                posts.push({url, opt});
                return netFails ? Promise.reject(new Error('net')) : Promise.resolve({ok: status < 300, status});
            }
        });
        await ctx.vmSend(input);
        assert.equal(input.value, '');  // the same file can be picked again
        return {msgs, posts, closed, last: msgs[msgs.length - 1], ctx};
    }
    const bytes = p => p.opt.body.parts[0].byteLength;

    let o = await send({});
    assert.equal(o.posts.length, 1);
    assert.equal(o.posts[0].url, '/api/memo?from=Parent');
    assert.equal(o.posts[0].opt.method, 'POST');
    assert.equal(bytes(o.posts[0]), 44 + 2 * 16000 * 2);
    assert.equal(o.last, 'Message sent.');
    assert.equal(o.closed, 1);

    o = await send({duration: 75});
    assert.equal(bytes(o.posts[0]), 44 + 60 * 16000 * 2);   // cut, under the 2 MB cap
    assert.equal(o.last, 'Message sent (cut to 60 s).');

    o = await send({offlineThrows: true, rate: 48000});
    assert.equal(bytes(o.posts[0]), 44 + 2 * 16000 * 2);
    assert.equal(o.last, 'Message sent.');

    o = await send({decodeFails: true});
    assert.equal(o.posts.length, 0);
    assert.equal(o.last, 'This audio file cannot be read by the browser.');

    o = await send({status: 507});
    assert.equal(o.last, o.ctx.vmErr(507));
    o = await send({netFails: true});
    assert.equal(o.last, o.ctx.vmErr(0));

    o = await send({from: 'Maman & Papa'});
    assert.equal(o.posts[0].url, '/api/memo?from=Maman%20%26%20Papa');
    o = await send({from: '  '});
    assert.equal(o.posts[0].url, '/api/memo?from=Parent');

    console.log('web voice message: wav, resample, status messages, send pipeline passed');
}
main().catch(e => { console.error(e); process.exit(1); });
