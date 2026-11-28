// Connects the JavaScript engine to a page: the DOM as script sees it, events, timers and window objects.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dom.h"

namespace tb {

struct ScriptSource {
    enum Kind { Classic, Module, ImportMap };
    Node* node = nullptr;  // the <script> element
    std::string code;      // UTF-8 source text; empty for a module that is fetched from `url`
    std::string url;       // where it came from, for error messages
    Kind kind = Classic;
    bool external = false;  // has a src attribute
    bool deferred = false;  // runs once the document is parsed, in document order: defer scripts and modules
    bool async = false;     // runs as soon as it is ready
};

class ScriptHost {
public:
    // What the scripting layer needs from the browser shell. All calls happen on the UI thread.
    struct Client {
        virtual ~Client() = default;
        virtual void domChanged() = 0;  // the tree, attributes or text changed: restyle and lay out again
        virtual void consoleMessage(int level, const std::wstring& text) = 0;
        virtual void navigate(const std::string& url, bool replace) = 0;
        virtual void submitForm(Node* form, Node* submitter) = 0;  // carried out after the script returns
        virtual void wake(double delayMs) = 0;  // call pump() after this long
        virtual std::function<void()> threadWake() = 0;  // callable from any thread: asks for a pump() soon
        virtual void ensureLayout() = 0;        // bring element boxes up to date before script reads them
        virtual void viewport(float& w, float& h, float& scrollY) = 0;
        virtual void scrollTo(float y) = 0;
        virtual void elementScrolled() = 0;     // an element's scroll offset changed: lay out again
        virtual std::wstring formValue(Node* n) = 0;
        virtual void setFormValue(Node* n, const std::wstring& v) = 0;
        virtual bool formChecked(Node* n) = 0;
        virtual void setFormChecked(Node* n, bool on) = 0;
        virtual void focusNode(Node* n) = 0;
        virtual Node* focusedNode() = 0;
        virtual void imageSourceChanged(Node* img) = 0;  // a script gave an <img> another source: fetch it, then fire load or error
        // The picture an <img> shows: its own size, 0 by 0 when it has none or failed. False while it is still loading.
        virtual bool imageSize(Node* img, int& w, int& h) = 0;
        virtual void titleChanged() = 0;
        // The document's URL changed without a load (pushState, replaceState): `push` adds a session history entry.
        virtual void urlChanged(const std::string& url, bool push) = 0;
        virtual int historyLength() = 0;       // how many entries the tab's session history has
        virtual void traverse(int delta) = 0;  // history.go(delta): carried out after the script returns
        // Whether what the window's load event waits for (HTML 8.4.1: style sheets and what they import, pictures)
        // is still on its way
        virtual bool loadBlocked() { return false; }
        // The decoded picture of an <img>: non-premultiplied RGBA rows from the top. 1, or 0 when it has none (yet),
        // 2 when it came from another origin than the document's (a script may not read it)
        virtual int imagePixels(Node* img, int& w, int& h, std::vector<uint8_t>& rgba) { return 0; }
        // The same picture as the page keeps it: premultiplied BGRA rows, valid until the script returns, and a
        // number that is the same for the same picture
        virtual int imageBGRA(Node* img, int& w, int& h, const uint8_t*& bgra, int& id) { return 0; }
        // A canvas has new contents (a WebGL context drew): premultiplied BGRA rows from the top
        virtual void canvasPainted(Node* canvas, int w, int h, std::vector<uint8_t>&& bgra) {}
    };

    ScriptHost(Document& doc, const std::string& url, Client& client);
    ~ScriptHost();

    // Runs the page's scripts in document order, then fires DOMContentLoaded, and load once nothing holds it back.
    void runScripts(const std::vector<ScriptSource>& scripts);
    void fireLoadEvents();
    // Fires a user-interface event at a node (null: the window). Returns false when a listener cancelled it.
    bool dispatch(Node* target, const std::string& type, bool bubbles = true, bool cancelable = true);
    // An AnimationEvent or TransitionEvent (CSS Animations 1, 4.1; CSS Transitions 1, 6.1): animationName or
    // propertyName, and elapsedTime in seconds
    void animationEvent(Node* target, const std::string& type, const std::string& name, double elapsed);
    // The browser is about to navigate the document away or to a fragment of it (a link, a location change): the
    // navigate event of the Navigation API (HTML 7.4.2.3). 0 go on, 1 a listener cancelled it, 2 a listener
    // intercepted it and the URL has already changed without a load. type is "push" or "replace".
    int beforeNavigate(const std::string& url, const std::string& type, bool userInitiated);
    // Fires the submit event (a SubmitEvent naming the submitter, HTML 4.10.21.3) at a form. False when cancelled.
    bool submitEvent(Node* form, Node* submitter);
    // Runs timers that are due. The shell calls this when a wake() delay has passed.
    void pump();
    // The tab went to another session history entry of this document, which loads nothing (HTML 7.4.6.2): the entry
    // at `index` among the document's own entries. Fires popstate, and hashchange when the fragment changed.
    void traversed(size_t index);
    // The tab navigated to a fragment of this document (HTML 7.4.2.3.3): a new entry when `push`, otherwise the
    // current one replaced. Fires popstate and hashchange.
    void fragmentNavigated(const std::string& url, bool push);
    bool restoresScroll() const;  // history.scrollRestoration is "auto"
    // Devtools console: evaluates in the page's global scope. Returns false and the error text on failure.
    bool eval(const std::wstring& source, std::wstring& out);
    // Style sheets that scripts made and adopted (CSSOM 6.2, adoptedStyleSheets): the text of each with the tree
    // it styles, a shadow root or null for the document.
    std::vector<std::pair<const Node*, std::string>> adoptedStyles() const;
    bool stopped() const;  // scripts were halted (time limit)
    bool busy() const;     // network requests started by script are still in flight
    // Time the browser spent on its own work (styling, layout) while page script was running: it does not count
    // towards the limit that stops scripts which run too long
    void hostTime(uint64_t ms);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace tb
