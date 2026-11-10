// Web page: a new radio or podcast never reuses an id that was deleted in
// the same page session. The id is identity on the device (podcast manifest
// <id>.json, new-episode markers, the alarm's radio_id): with "highest id on
// the page + 1", deleting the last row and adding one in the same save gave
// the new entry the deleted one's id, and its leftover manifest with it.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../components/web_config/www/index.html'), 'utf8');
function extract(name) {
    const start = html.indexOf('function ' + name + '(');
    assert(start >= 0, name);
    let end = html.indexOf('{', start), depth = 1;
    while (depth) {
        end++;
        if (html[end] === '{') depth++;
        if (html[end] === '}') depth--;
    }
    return html.slice(start, end + 1);
}
let rows = [];
const row = (id, url) => ({dataset: {id: id ? String(id) : ''}, querySelector: () => ({value: url})});
const ctx = vm.createContext({
    document: {querySelectorAll: () => rows},
    alert() {}, tr: s => s,
});
vm.runInContext('var idHi={};' + extract('idSeed') + extract('collect'), ctx);
const PF = [{k: 'rss_url'}];
const ids = () => Array.from(ctx.collect('podcasts', PF), o => o.id);

// Loaded config has ids 1..5. The user deletes 5 and adds a row, then saves.
ctx.idSeed('podcasts', [{id: 1}, {id: 2}, {id: 5}]);
rows = [row(1, 'a'), row(2, 'b'), row(0, 'new')];
assert.deepEqual(ids(), [1, 2, 6]);
// A later save in the same session keeps counting up, even with 6 gone too.
rows = [row(1, 'a'), row(0, 'newer')];
assert.deepEqual(ids(), [1, 7]);
// Radios count separately, and an unseeded list starts at 1.
rows = [row(0, 'r')];
assert.deepEqual(Array.from(ctx.collect('radios', [{k: 'url'}]), o => o.id), [1]);
// Existing ids are never renumbered.
rows = [row(9, 'x'), row(3, 'y')];
assert.deepEqual(ids(), [9, 3]);
console.log('web ids: no id reuse within a page session passed');
