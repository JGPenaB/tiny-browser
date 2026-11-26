// Engine self-tests: `tinybrowser.exe --selftest [out.txt]`. Exit code 0 when all pass.
#include <windows.h>

#include <cstdio>
#include <string>

#include "css.h"
#include "html.h"
#include "storage.h"
#include "url.h"

namespace tb {

namespace {
struct T {
    int pass = 0, fail = 0;
    std::string log;
    void check(bool ok, const std::string& what) {
        (ok ? pass : fail)++;
        log += (ok ? "PASS  " : "FAIL  ") + what + "\n";
    }
};

Node* find(Node* n, const std::string& tag, int nth = 0) {
    std::vector<Node*> st{n};
    while (!st.empty()) {
        Node* x = st.back();
        st.pop_back();
        if (x->is(tag) && nth-- == 0) return x;
        for (auto it = x->kids.rbegin(); it != x->kids.rend(); ++it) st.push_back(it->get());
    }
    return nullptr;
}
}  // namespace

int run_selftest(const std::wstring& outPath) {
    T t;
    // URL resolution (RFC 3986 examples).
    const char* base = "http://a/b/c/d;p?q";
    const char* cases[][2] = {{"g", "http://a/b/c/g"},       {"./g", "http://a/b/c/g"},   {"g/", "http://a/b/c/g/"},
                              {"/g", "http://a/g"},          {"//g", "http://g/"},        {"?y", "http://a/b/c/d;p?y"},
                              {"g?y", "http://a/b/c/g?y"},   {"#s", "http://a/b/c/d;p?q#s"}, {"../g", "http://a/b/g"},
                              {"../../g", "http://a/g"},     {"../../../g", "http://a/g"}, {".", "http://a/b/c/"},
                              {"https://x.org:443/a", "https://x.org/a"}};
    for (auto& c : cases) {
        std::string got = resolve_url(base, c[0]);
        t.check(got == c[1], std::string("resolve ") + c[0] + " -> " + got);
    }

    // HTML tree construction.
    {
        auto d = parse_html("<p>a<p>b<ul><li>1<li>2</ul><table><tr><td>x<td>y</table>", "about:test");
        Node* body = d->body;
        t.check(body && body->kids.size() == 4, "implied </p>, body has p,p,ul,table (" + std::to_string(body ? body->kids.size() : 0) + ")");
        Node* ul = find(d->root.get(), "ul");
        t.check(ul && ul->kids.size() == 2, "implied </li>");
        Node* tr = find(d->root.get(), "tr");
        t.check(tr && tr->kids.size() == 2, "implied </td>");
        t.check(d->head != nullptr && d->html != nullptr, "implied html/head/body");
    }
    {
        auto d = parse_html("<title>T &amp; &lt;x&gt; &#x41;&eacute;&copy2024</title><script>if(a<b){}</script>", "about:test");
        t.check(d->title == "T & <x> A\xC3\xA9\xC2\xA9" "2024", "entities + rcdata title: " + d->title);
        Node* s = find(d->root.get(), "script");
        t.check(s && !s->kids.empty() && s->kids[0]->text == "if(a<b){}", "script is raw text");
    }
    {
        auto d = parse_html("<div id=a class='x  y' data-v=\"1\">t</div><img src=a.png><br/>", "about:test");
        Node* div = find(d->root.get(), "div");
        t.check(div && div->id == "a" && div->classes.size() == 2, "id + class parsing");
        Node* img = find(d->root.get(), "img");
        t.check(img && img->kids.empty() && img->parent == d->body, "void element");
    }

    // CSS parsing + cascade.
    {
        auto d = parse_html(
            "<style>p{color:red} #x{color:#00f} .c{color:green!important} div>p{margin:1em 2px} "
            ":root{--k:5px} span{padding:var(--k)} @media (max-width:100px){p{color:yellow}} li:nth-child(2n){font-weight:bold}</style>"
            "<div><p id=x>t</p><p class=c id=x2 style='color:black'>u</p><span>s</span></div><ul><li>a<li>b</ul>",
            "about:test");
        Sheet sh = parse_sheet(text_content(find(d->root.get(), "style")));
        StyleEngine se;
        se.add(&sh, false);
        se.compute(*d, 800, 600);
        Node* p1 = find(d->root.get(), "p", 0);
        Node* p2 = find(d->root.get(), "p", 1);
        Node* sp = find(d->root.get(), "span");
        Node* li2 = find(d->root.get(), "li", 1);
        auto approx = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        t.check(p1 && p1->style && approx(p1->style->color.b, 1) && approx(p1->style->color.r, 0), "id beats type selector");
        t.check(p2 && p2->style && approx(p2->style->color.g, 128 / 255.f), "!important beats inline style");
        t.check(p1 && approx(p1->style->margin[0].v, 16) && approx(p1->style->margin[1].v, 2), "child combinator + em units");
        t.check(sp && sp->style && approx(sp->style->padding[0].v, 5), "custom property var()");
        t.check(li2 && li2->style && li2->style->weight == 700, ":nth-child(2n)");
        Color c;
        t.check(parse_color("rgb(10 20 30 / 50%)", c) && approx(c.a, 0.5f) && approx(c.r, 10 / 255.f), "modern rgb() syntax");
        t.check(parse_color("hsl(120, 100%, 50%)", c) && approx(c.g, 1) && approx(c.r, 0), "hsl()");
    }

    // DPAPI password vault round trip.
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring path = std::wstring(tmp) + L"tinybrowser_selftest_vault.dat";
        DeleteFileW(path.c_str());
        {
            PasswordVault v(path);
            v.load();
            v.put("https://example.com", "alice", "s3cret-P4ss!");
            v.put("https://example.org", "bob", "hunter2");
            t.check(v.save(), "vault save");
        }
        std::string raw;
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.append(buf, n);
            fclose(f);
        }
        t.check(!raw.empty() && raw.find("s3cret") == std::string::npos && raw.find("alice") == std::string::npos,
                "vault file is encrypted (no plaintext)");
        PasswordVault v2(path);
        t.check(v2.load(), "vault load");
        auto c = v2.find("https://example.com", "alice");
        t.check(c && c->pass == "s3cret-P4ss!" && v2.all().size() == 2, "vault round trip");
        DeleteFileW(path.c_str());
    }

    t.log += "\n" + std::to_string(t.pass) + " passed, " + std::to_string(t.fail) + " failed\n";
    if (FILE* f = _wfopen(outPath.c_str(), L"w")) {
        fputs(t.log.c_str(), f);
        fclose(f);
    }
    return t.fail == 0 ? 0 : 1;
}

}  // namespace tb
