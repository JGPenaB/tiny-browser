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
        virtual std::wstring formValue(Node* n) = 0;
        virtual void setFormValue(Node* n, const std::wstring& v) = 0;
        virtual bool formChecked(Node* n) = 0;
        virtual void setFormChecked(Node* n, bool on) = 0;
        virtual void focusNode(Node* n) = 0;
        virtual Node* focusedNode() = 0;
        virtual void titleChanged() = 0;
        virtual void urlChanged(const std::string& url) = 0;
    };

    ScriptHost(Document& doc, const std::string& url, Client& client);
    ~ScriptHost();

    // Runs the page's scripts in document order, then fires DOMContentLoaded and load.
    void runScripts(const std::vector<ScriptSource>& scripts);
    void fireLoadEvents();
    // Fires a user-interface event at a node (null: the window). Returns false when a listener cancelled it.
    bool dispatch(Node* target, const std::string& type, bool bubbles = true, bool cancelable = true);
    // Runs timers that are due. The shell calls this when a wake() delay has passed.
    void pump();
    // Devtools console: evaluates in the page's global scope. Returns false and the error text on failure.
    bool eval(const std::wstring& source, std::wstring& out);
    // Style sheets that scripts made and adopted (CSSOM 6.2, adoptedStyleSheets): the text of each with the tree
    // it styles, a shadow root or null for the document.
    std::vector<std::pair<const Node*, std::string>> adoptedStyles() const;
    bool stopped() const;  // scripts were halted (time limit)
    bool busy() const;     // network requests started by script are still in flight

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace tb
