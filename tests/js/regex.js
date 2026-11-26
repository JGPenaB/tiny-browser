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
done();
