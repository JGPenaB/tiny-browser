test("regex basics", function () {
  eq(/abc/.test("xabcx"), true); eq(/abc/.test("ab"), false); eq(/^abc$/.test("abc"), true); eq(/a.c/.test("a\nc"), false); eq(/a.c/s.test("a\nc"), true);
  eq("hello world".match(/o w/).index, 4); eq(/(\d+)-(\d+)/.exec("tel 555-1234 x").slice(), ["555-1234", "555", "1234"]);
  eq(/a|b|c/.exec("xxc")[0], "c"); eq(/(a)|(b)/.exec("b").slice(), ["b", undefined, "b"]); eq(/x*/.exec("aaa")[0], ""); eq(/a+?/.exec("aaa")[0], "a"); eq(/a{2,3}/.exec("aaaa")[0], "aaa");
  eq(/a{2}/.test("a"), false); eq(/a{2,}/.exec("aaaaa")[0], "aaaaa"); eq(/a{1,3}?b/.exec("aaab")[0], "aaab"); eq(/colou?r/.test("color"), true);
  eq(/[a-c]+/.exec("xxabcabcx")[0], "abcabc"); eq(/[^a-c]+/.exec("abcxyzabc")[0], "xyz"); eq(/[\d.]+/.exec("v1.25.3b")[0], "1.25.3"); eq(/[\]\\-]+/.exec("a]\\-b")[0], "]\\-");
  eq(/\bfoo\b/.test("a foo b"), true); eq(/\bfoo\b/.test("afoob"), false); eq(/\Boo/.test("foo"), true); eq(/\w+@\w+\.\w+/.exec("mail bob@example.com now")[0], "bob@example.com");
  eq(/\s+/.exec("a \t\n b")[0], " \t\n "); eq(/\S+/.exec("  word  ")[0], "word"); eq(/\D+/.exec("12ab34")[0], "ab"); eq(/\W/.exec("ab_c d")[0], " ");
  eq(/\x41B\t/.test("AB\t"), true); eq(/\//.test("a/b"), true); eq(/[/]/.test("a/b"), true); eq(/\$\^\.\*/.test("$^.*"), true); eq(/a\.b/.test("axb"), false);
  eq(/(?:ab)+/.exec("ababab")[0], "ababab"); eq(/(ab)+/.exec("ababab")[1], "ab"); eq(/(a(b(c)))/.exec("abc").slice(), ["abc", "abc", "bc", "c"]);
  eq(/^$/.test(""), true); eq(/^/.test("abc"), true); eq(/$/.exec("abc").index, 3); eq(/(?:)/.exec("abc")[0], "");
  eq(/[]/.test("a"), false); eq(/[^]/.test("\n"), true); eq(/a]/.test("a]"), true); eq(/{/.test("{"), true); eq(/a{,2}/.test("a{,2}"), true); eq(/\1(a)/.exec("aa")[0], "a");
});
test("regex flags and state", function () {
  eq(/ABC/i.test("xabcx"), true); eq(/[a-z]+/i.exec("HeLLo")[0], "HeLLo"); eq(/é/i.test("É"), true); eq(/é/i.test("É"), true);
  eq("a\nb\nc".match(/^\w$/gm), ["a", "b", "c"]); eq(/^b$/.test("a\nb\nc"), false); eq(/^b$/m.test("a\nb\nc"), true);
  const g = /o/g; eq([g.test("foo"), g.lastIndex, g.test("foo"), g.lastIndex, g.test("foo"), g.lastIndex], [true, 2, true, 3, false, 0]);
  const y = /foo/y; y.lastIndex = 3; eq(y.test("barfoo"), true); y.lastIndex = 2; eq(y.test("barfoo"), false); eq(y.lastIndex, 0);
  const re = /(\d)/g; const found = []; let m; while ((m = re.exec("a1b22c"))) found.push(m[1] + "@" + m.index); eq(found, ["1@1", "2@3", "2@4"]);
  eq([/a/gi.flags, /a/gi.global, /a/.global, /a/m.multiline, /a/y.sticky, /a/s.dotAll, /a/u.unicode, /a\/b/.source, String(/a\/b/gi), new RegExp("").source], ["gi", true, false, true, true, true, true, "a\\/b", "/a\\/b/gi", "(?:)"]);
  eq(new RegExp("a+", "g").exec("caat")[0], "aa"); eq(new RegExp(/x/g, "i").flags, "i"); eq(new RegExp("\\d+").test("a1"), true);
});
test("regex errors", function () {
  throws(() => new RegExp("("), SyntaxError); throws(() => new RegExp("a**"), SyntaxError); throws(() => new RegExp("[b-a]"), SyntaxError); throws(() => new RegExp("a", "gg"), SyntaxError);
  throws(() => new RegExp("a", "q"), SyntaxError); throws(() => new RegExp("(?<n>a)\\k<m>"), SyntaxError); throws(() => new RegExp("\\"), SyntaxError); throws(() => new RegExp("a{2,1}"), SyntaxError);
  throws(() => new RegExp(")"), SyntaxError); throws(() => new RegExp("\\u{110000}", "u"), SyntaxError);
});
test("regex groups, lookaround, backreferences", function () {
  const d = /(?<year>\d{4})-(?<month>\d{2})-(?<day>\d{2})/.exec("on 2024-02-29!"); eq([d.groups.year, d.groups.month, d.groups.day, d.index], ["2024", "02", "29", 3]);
  eq("2024-02-29".replace(/(?<y>\d+)-(?<m>\d+)-(?<d>\d+)/, "$<d>/$<m>/$<y>"), "29/02/2024"); eq(/(?<q>['"]).*?\k<q>/.exec(`say "hi" 'x'`)[0], '"hi"');
  eq(/(a)\1/.test("aa"), true); eq(/(a)\1/.test("ab"), false); eq(/(['"])(.*?)\1/.exec(`x 'y' z`)[2], "y"); eq(/(a)|\1b/.exec("b")[0], "b");
  eq(/foo(?=bar)/.exec("foobar")[0], "foo"); eq(/foo(?=bar)/.test("foobaz"), false); eq(/foo(?!bar)/.test("foobaz"), true); eq(/foo(?!bar)/.test("foobar"), false);
  eq(/(?<=\$)\d+/.exec("cost $42")[0], "42"); eq(/(?<!\$)\b\d+/.exec("$42 and 17")[0], "17"); eq(/(?<=a.*)b/.exec("xaxxb").index, 4); eq("a1b2".replace(/(?<=[a-z])\d/g, "#"), "a#b#");
  eq(/(?=(\w+))\1:/.exec("abc:")[0], "abc:"); eq(/^(?:(a)|b)*$/.exec("ab").slice(), ["ab", undefined]); eq(/(z)((a+)?(b+)?(c))*/.exec("zaacbbbcac").slice(), ["zaacbbbcac", "z", "ac", "a", undefined, "c"]);
  eq(/(a*)*/.exec("b").slice(), ["", undefined]); eq(/(a*)+/.exec("aa").slice(), ["aa", "aa"]); eq(/(?:a?)*?b/.exec("aab")[0], "aab"); eq(/(|a)+/.exec("aaa")[0], "aaa");
  eq(/<(\w+)[^>]*>(.*?)<\/\1>/.exec('<b class="x">bold</b>').slice(1), ["b", "bold"]); eq(/^(?!.*\d)[a-z]+$/.test("abc"), true); eq(/^(?!.*\d)[a-z]+$/.test("ab1"), false);
  eq(/^(?=.*[A-Z])(?=.*\d).{6,}$/.test("Passw0rd"), true); eq(/(\d+)\s*(px|em|%)/.exec("width: 100 px;").slice(1), ["100", "px"]);
});
test("regex with strings", function () {
  eq("a-b_c d".split(/[-_ ]/), ["a", "b", "c", "d"]); eq("abc".split(/(?:)/), ["a", "b", "c"]); eq("a1b2c".split(/(\d)/), ["a", "1", "b", "2", "c"]); eq("a, b,c".split(/\s*,\s*/), ["a", "b", "c"]);
  eq("abc".split(/b*/), ["a", "c"]); eq("".split(/x/), [""]); eq("".split(/(?:)/), []); eq("a,b,c".split(/,/, 2), ["a", "b"]); eq("ab".split(/$/), ["ab"]);
  eq("aaa".replace(/a/, "b"), "baa"); eq("aaa".replace(/a/g, "b"), "bbb"); eq("John Smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "Smith, John"); eq("abc".replace(/b/, "[$`|$&|$']"), "a[a|b|c]c");
  eq("x-1 y-22".replace(/(\w)-(\d+)/g, (m, a, n, off) => a.toUpperCase() + (+n * 2) + "@" + off), "X2@0 Y44@4"); eq("abc".replace(/(?:)/g, "-"), "-a-b-c-"); eq("aXbXc".replaceAll(/x/gi, "."), "a.b.c");
  eq("camelCaseString".replace(/([A-Z])/g, " $1").toLowerCase(), "camel case string"); eq("  trim me  ".replace(/^\s+|\s+$/g, ""), "trim me"); eq("a.b.c".replace(/\./g, "/"), "a/b/c");
  eq("test1 test22".match(/\d+/g), ["1", "22"]); eq("none".match(/\d+/g), null); eq("x".match(/(y)?/).slice(), ["", undefined]); eq("abc".search(/c/), 2); eq("abc".search(/z/), -1);
  eq([..."a1b2".matchAll(/[a-z](\d)/g)].map(m => m[1] + m.index), ["10", "22"]); throws(() => "a".matchAll(/a/), TypeError); throws(() => "a".replaceAll(/a/, "b"), TypeError);
  eq("%20and%2F".replace(/%([0-9A-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))), " and/"); eq("k1=v1&k2=v2".split("&").map(p => p.split("=")), [["k1", "v1"], ["k2", "v2"]]);
  eq("The Quick  brown".split(/\s+/).map(w => w[0].toUpperCase() + w.slice(1).toLowerCase()).join(" "), "The Quick Brown"); eq("abc".match("b").index, 1); eq("a+b".split("+"), ["a", "b"]);
  eq("1,234,567.89".replace(/,/g, ""), "1234567.89"); eq("1234567.891".replace(/\B(?=(\d{3})+(?!\d))/g, ","), "1,234,567.891");
  eq(JSON.stringify("id: 42".match(/(?<key>\w+): (?<val>\d+)/).groups), '{"key":"id","val":"42"}'); eq(RegExp.escape(".b*c a"), "\\.b\\*c\\x20a"); eq(RegExp.escape("a1"), "\\x611");
});
test("regex unicode", function () {
  eq(/^.$/.test("😀"), false); eq(/^.$/u.test("😀"), true); eq("😀".match(/./gu).length, 1); eq(/\u{1F600}/u.test("😀"), true); eq(/[😀-😂]/u.test("😁"), true);
  eq(/\p{L}+/u.exec("héllo wörld")[0], "héllo"); eq(/\p{Lu}/u.exec("abcDef")[0], "D"); eq(/\p{N}+/u.exec("abc123")[0], "123"); eq(/\P{L}+/u.exec("ab, cd")[0], ", ");
  eq(/[\p{L}\d_]+/u.exec("__añb1!")[0], "__añb1"); eq(/\p{Script=Greek}+/u.exec("abc αβγ")[0], "αβγ"); eq(/\p{sc=Cyrillic}/u.test("ж"), true); eq(/\p{Emoji}/u.test("😀"), true);
  eq("naïve café".match(/[\wÀ-ÿ]+/g), ["naïve", "café"]); eq(/\w/.test("é"), false); eq("日本語".match(/[一-鿿]+/)[0], "日本語"); eq(/\s/.test(" "), true);
});
test("regex performance and limits", function () {
  const long = "a".repeat(200000) + "b"; const t0 = Date.now(); eq(/a*b/.test(long), true); eq(/^(?:a)*b$/.test(long), true); eq(long.replace(/a/g, "").length, 1); eq(long.match(/a+/)[0].length, 200000);
  const words = ("lorem ipsum dolor sit amet ").repeat(5000); eq(words.match(/\b\w+\b/g).length, 25000); eq(words.split(/\s+/).length, 25001); eq(words.replace(/(\w+) (\w+)/g, "$2 $1").length, words.length);
  const t1 = Date.now() - t0; console.log("timings ms: regex=" + t1); ok(t1 < 4000, "regex too slow: " + t1);
  eq(/(x+x+)+y/.test("x".repeat(40)), false, "catastrophic backtracking is cut off");
});
test("the RegExp protocol behind String methods", function () {
  const calls = []; const fake = { [Symbol.replace](s, r) { calls.push("replace", s, r); return "R"; }, [Symbol.match](s) { calls.push("match"); return "M"; }, [Symbol.search](s) { return 7; }, [Symbol.split](s, l) { return [s, l]; } };
  eq(["abc".replace(fake, "x"), "abc".match(fake), "abc".search(fake), "abc".split(fake, 2), calls], ["R", "M", 7, ["abc", 2], ["replace", "abc", "x", "match"]]);
  class Loud extends RegExp { exec(s) { const m = super.exec(s); if (m) m[0] = m[0].toUpperCase(); return m; } }
  eq(["a-b-c".replace(new Loud("[a-c]", "g"), "<$&>"), "abc".match(new Loud("b"))[0], new Loud("b").test("abc"), "a1b2".split(new Loud("[0-9]")), [..."a1b2".matchAll(new Loud("[0-9]", "g"))].map(m => m[0])], ["<A>-<B>-<C>", "B", true, ["a", "b", ""], ["1", "2"]]);
  const re = /b/g; re.exec = function () { return null; }; eq(["abc".replace(re, "x"), re.test("abc")], ["abc", false]);
  eq([RegExp.prototype[Symbol.replace].call(/(?<n>b)/, "abc", "[$<n>$1$`$'$$]"), RegExp.prototype[Symbol.match].call(/./g, "ab"), RegExp.prototype[Symbol.search].call(/c/, "abc"), RegExp.prototype[Symbol.split].call(/,/, "a,b", 1)], ["a[bbac$]c", ["a", "b"], 2, ["a"]]);
  const it = /a/g[Symbol.matchAll]("aXa"); eq([Object.prototype.toString.call(it), it.next().value.index, it.next().value.index, it.next().done], ["[object RegExp String Iterator]", 0, 2, true]);
  throws(() => "a".matchAll(/a/), TypeError); throws(() => "a".replaceAll(/a/, "b"), TypeError); throws(() => "a".startsWith(/a/), TypeError); throws(() => RegExp.prototype[Symbol.match].call(1, "a"), TypeError);
  const notRe = /a/; notRe[Symbol.match] = false; eq("/a/".startsWith(notRe), true);
});
test("flags, lastIndex and friends", function () {
  eq([/a/dgimsuy.flags, /a/v.unicodeSets, /a/.unicodeSets, RegExp.prototype.flags, RegExp.prototype.global, RegExp.prototype.source, Object.getOwnPropertyDescriptor(RegExp.prototype, "flags").get.call({ global: 1, sticky: 1, unicode: 0 }), RegExp[Symbol.species] === RegExp], ["dgimsuy", true, false, "", undefined, "(?:)", "gy", true]);
  throws(() => new RegExp("a", "uv"), SyntaxError); throws(() => new RegExp("a", "gg"), SyntaxError); throws(() => Object.getOwnPropertyDescriptor(RegExp.prototype, "global").get.call({}), TypeError);
  const frozen = Object.freeze(/a/g); throws(() => frozen.exec("a"), TypeError); eq(Object.freeze(/a/).exec("a")[0], "a");
  const li = /a/g; li.lastIndex = { valueOf() { return 1; } }; eq([li.exec("aaa").index, li.lastIndex], [1, 2]); li.lastIndex = 5; eq([li.test("aaa"), li.lastIndex], [false, 0]);
  const d = /(?<x>b)(c)?/d.exec("abd"); eq([d.indices[0], d.indices[1], d.indices[2], d.indices.groups.x, Object.keys(d)], [[1, 2], [1, 2], undefined, [1, 2], ["0", "1", "2", "index", "input", "groups", "indices"]]);
  eq([RegExp(/a/g) instanceof RegExp, (() => { const r = /a/g; return RegExp(r) === r; })(), new RegExp(/a/g, "i").flags, new RegExp({ [Symbol.match]: true, source: "x", flags: "g", constructor: RegExp }).source, String(new RegExp("/"))], [true, true, "i", "x", "/\\//"]);
  const c = /a/g; c.lastIndex = 3; eq([c.compile("b", "i") === c, c.source, c.flags, c.lastIndex, c.test("B")], [true, "b", "i", 0, true]);
  /([0-9])([a-z])/.exec("x1yz"); eq([RegExp.$1, RegExp.$2, RegExp.$3, RegExp.lastMatch, RegExp["$&"], RegExp.leftContext, RegExp.rightContext, RegExp.lastParen, RegExp.input], ["1", "y", "", "1y", "1y", "x", "z", "y", "x1yz"]);
  "ab".replace(/b/, "c"); eq([RegExp.lastMatch, RegExp["$`"]], ["b", "a"]); throws(() => Object.getOwnPropertyDescriptor(RegExp, "$1").get.call({}), TypeError);
  eq(["x".anchor('a"b'), "x".bold(), "x".link("u"), "x".fontsize(3), "x".sup(), "Å".normalize() === "Å", "Å".normalize("NFD").length, "x".padEnd.length], ['<a name="a&quot;b">x</a>', "<b>x</b>", '<a href="u">x</a>', '<font size="3">x</font>', "<sup>x</sup>", true, 2, 1]);
  throws(() => "x".normalize("bad"), RangeError);
});
test("the v flag: sets with operations and strings (22.2.1)", function () {
  eq([/^[[a-z]--[aeiou]]+$/v.test("xyz"), /^[[a-z]--[aeiou]]+$/v.test("xay"), /^[\w&&[^\d]]+$/v.test("ab_"), /^[\w&&[^\d]]+$/v.test("a1")], [true, false, true, false]);
  eq([/^[\p{L}&&\p{ASCII}]+$/v.test("abc"), /^[\p{L}&&\p{ASCII}]+$/v.test("aé"), /^[[0-9][a-f]]+$/v.test("09af"), /[\q{abc|d|}]/v.exec("xabc")[0]], [true, false, true, ""]);
  eq([/^[\q{abc|ab|a}x]$/v.exec("abc")[0], /[\q{ab|abc}]/v.exec("abcd")[0], /^[\p{Emoji_Keycap_Sequence}_]+$/v.test("1️⃣_"), /^\p{Emoji_Keycap_Sequence}$/v.test("1")], ["abc", "abc", true, false]);
  eq([/[a\-b]/v.test("-"), /[\&\&]/v.test("&"), new RegExp("[a-c]", "v").unicodeSets, /./v.flags], [true, true, true, "v"]);
  for (const src of ["[a&&&b]", "[a--b&&c]", "[ab--c]", "[(]", "[a-]", "[^\\q{ab}]", "[\\P{Emoji_Keycap_Sequence}]", "[a||b]", "[&&a]"]) throws(() => new RegExp(src, "v"), SyntaxError, src);
  throws(() => new RegExp("\\ ", "u"), SyntaxError); throws(() => new RegExp("\\-", "u"), SyntaxError); eq(/[\-]/u.test("-"), true);
});
test("lookbehind matches right to left (22.2.2.3)", function () {
  eq(/(?<=(\d+)(\d+))$/.exec("1053").slice(1), ["1", "053"]);
  eq(/(?<=([abc])\d+)x/.exec("a123x").slice(1), ["a"]);
  eq(/(?<=\1(a))b/.exec("aab").slice(0), ["b", "a"]);
  eq(/(?<=(?<a>\w){3})f/.exec("abcdef").groups.a, "c");
  eq(["abcd".replace(/(?<=b)c/, "X"), "abcd".replace(/(?<!b)c/, "X"), /(?<=^ab)c/.test("abc"), /(?<=\$)\d+(\.\d*)?/.exec("cost $10.50")[0]], ["abXd", "abcd", true, "10.50"]);
  eq([/(?<=\u{1F600})x/u.test("\u{1F600}x"), /(?<=a(?=b)b)c/.test("abc"), /(?<=(?<!x)a)b/.test("ab"), "a1b2".match(/(?<=[a-z])\d/g)], [true, true, true, ["1", "2"]]);
});
test("case-insensitive matching by the flag in force (22.2.5.6 Canonicalize)", function () {
  eq([/\u212a/i.test("k"), /\u212a/iu.test("k"), /\u212a/iu.test("K"), /k/iu.test("\u212a"), /[\u212a]/iu.test("k"), /\u017f/i.test("s"), /\u017f/iu.test("S")], [false, true, true, true, true, false, true]);
  eq([/[\u0390]/ui.test("\u1fd3"), /[\u1fd3]/ui.test("\u0390"), /[\ufb05]/ui.test("\ufb06"), /\u03c2/iu.test("\u03a3"), /\u00df/iu.test("\u1e9e"), /i/iu.test("\u0130")], [true, true, true, true, true, false]);
  eq([/(?=(abc))?a/.exec("abc").slice(), /(?:(a)|b)?c/.exec("c").slice(), /(a*)?b/.exec("b").slice()], [["a", undefined], ["c", undefined], ["b", undefined]]);
});
test("properties of strings: the emoji sequences (table 68)", function () {
  const family = "\u{1F468}‍\u{1F469}‍\u{1F467}‍\u{1F466}", flag = "\u{1F1EB}\u{1F1F7}", thumb = "\u{1F44D}\u{1F3FD}", keycap = "#️⃣";
  const england = "\u{1F3F4}\u{E0067}\u{E0062}\u{E0065}\u{E006E}\u{E0067}\u{E007F}", watch = "⌚", heart = "❤️";
  eq([/^\p{RGI_Emoji_ZWJ_Sequence}$/v.test(family), /^\p{RGI_Emoji_Flag_Sequence}$/v.test(flag), /^\p{RGI_Emoji_Modifier_Sequence}$/v.test(thumb),
    /^\p{Emoji_Keycap_Sequence}$/v.test(keycap), /^\p{RGI_Emoji_Tag_Sequence}$/v.test(england), /^\p{Basic_Emoji}$/v.test(watch), /^\p{Basic_Emoji}$/v.test(heart)],
    [true, true, true, true, true, true, true]);
  eq(/^\p{RGI_Emoji}+$/v.test(family + flag + thumb + keycap + england + watch + heart), true);
  eq([/^\p{RGI_Emoji_Flag_Sequence}$/v.test(family), /^\p{Basic_Emoji}$/v.test(flag), /^\p{RGI_Emoji}$/v.test("a"), /^\p{RGI_Emoji}$/v.test("\u{1F1EB}")], [false, false, false, false]);
  // the longest sequence is tried first, and set operations work on the strings
  eq([/\p{RGI_Emoji}/v.exec(family + "x")[0] === family, /^[\p{RGI_Emoji}--\p{RGI_Emoji_Flag_Sequence}]$/v.test(flag), /^[\p{RGI_Emoji}&&\p{Basic_Emoji}]$/v.test(watch)],
    [true, false, true]);
  for (const src of ["\\p{RGI_Emoji}", "\\p{Basic_Emoji}"]) throws(() => new RegExp(src, "u"), SyntaxError, src);
  for (const src of ["\\P{RGI_Emoji}", "[^\\p{RGI_Emoji}]", "\\p{RGI_Emoji_Sequence}"]) throws(() => new RegExp(src, "v"), SyntaxError, src);
});
done();
