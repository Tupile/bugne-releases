// Playlists card (Play tab): the helpers with no DOM access. The name rule
// must mirror playlist_name_valid() in components/playlist/playlist_m3u.c.
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
const code = ['plNameOk', 'plMove', 'plAppend', 'plErr'].map(extract).join('\n');
const ctx = vm.createContext({TextEncoder, tr: x => x});
vm.runInContext('var PL_MAX_TRACKS=128;' + code, ctx);

// Name rule: same cases as test_playlist_m3u.c.
for (const ok of ['Rock', "Chansons d'été 2026", 'a.b', 'a'.repeat(48)]) {
    assert.equal(ctx.plNameOk(ok), true, ok);
}
for (const bad of ['', '.hidden', 'end.', 'end ', 'a/b', 'a\\b', 'a:b', 'a*b', 'a?b', 'a"b',
                   'a<b', 'a>b', 'a|b', 'a\tb', 'a\x7fb', 'a'.repeat(49)]) {
    assert.equal(ctx.plNameOk(bad), false, JSON.stringify(bad));
}
// The limit is 48 BYTES of UTF-8, not 48 characters.
assert.equal(ctx.plNameOk('é'.repeat(24)), true, '24 two-byte characters');
assert.equal(ctx.plNameOk('é'.repeat(25)), false, '25 two-byte characters');
assert.equal(ctx.plNameOk(null), false);

// Move: swaps with the neighbour, returns a NEW array, no-op at the ends.
const t = [{path: 'a'}, {path: 'b'}, {path: 'c'}];
assert.deepEqual(ctx.plMove(t, 0, 1).map(x => x.path), ['b', 'a', 'c']);
assert.deepEqual(ctx.plMove(t, 2, -1).map(x => x.path), ['a', 'c', 'b']);
assert.deepEqual(ctx.plMove(t, 0, -1).map(x => x.path), ['a', 'b', 'c']);
assert.deepEqual(ctx.plMove(t, 2, 1).map(x => x.path), ['a', 'b', 'c']);
assert.deepEqual(t.map(x => x.path), ['a', 'b', 'c'], 'input untouched');

// Append: refuses a path already present and the 129th track.
let r = ctx.plAppend(t, {path: 'd', title: 'D'});
assert.equal(r.ok, true);
assert.deepEqual(r.tracks.map(x => x.path), ['a', 'b', 'c', 'd']);
assert.equal(t.length, 3, 'input untouched');
r = ctx.plAppend(t, {path: 'b', title: 'again'});
assert.equal(r.ok, false);
assert.equal(r.why, 'dup');
const full = Array.from({length: 128}, (_, i) => ({path: 'p' + i}));
r = ctx.plAppend(full, {path: 'new'});
assert.equal(r.ok, false);
assert.equal(r.why, 'full');

// Status messages follow playlist_result() in web_config.c: change both together.
const seen = new Set();
for (const code of [400, 404, 409, 413, 503, 507]) {
    const m = ctx.plErr(code);
    assert(m && m !== ctx.plErr(500), 'specific message for ' + code);
    seen.add(m);
}
assert.equal(seen.size, 6, 'one distinct message per status');
// Every message, and the card's static strings, exist in French and German.
for (const dict of ['WEB_FR', 'WEB_DE']) {
    const a = html.indexOf('var ' + dict + '={');
    const b = html.indexOf('};', a);
    const d = html.slice(a, b);
    for (const code of [400, 404, 409, 413, 500, 503, 507]) {
        assert(d.includes(JSON.stringify(ctx.plErr(code)) + ':'), dict + ' lacks ' + ctx.plErr(code));
    }
    for (const key of ['h_pl', 'hint_nopl', 'b_plnew', 'lbl_pltarget']) {
        assert(new RegExp('[{,]"?' + key + '"?:').test(d), dict + ' lacks ' + key);
    }
}
console.log('web playlists: name rule, move, append, status messages, translations passed');
