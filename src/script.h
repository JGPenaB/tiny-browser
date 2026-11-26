// Connects the JavaScript engine to a page: the DOM as script sees it, events, timers and window objects.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "dom.h"

namespace tb {

struct ScriptSource {
    Node* node = nullptr;  // the <script> element
    std::string code;      // UTF-8 source text
    std::string url;       // where it came from, for error messages
};

class ScriptHost {
public:
    // What the scripting layer needs from the browser shell. All calls happen on the UI thread.
    struct Client {
        virtual ~Client() = default;
        virtual void domChanged() = 0;  // the tree, attributes or text changed: restyle and lay out again
        virtual void consoleMessage(int level, const std::wstring& text) = 0;
        virtual void navigate(const std::string& url, bool replace) = 0;
        virtual void wake(double delayMs) = 0;  // call pump() after this long
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
    // Fires a user-interface event at a node (null: the window). Returns false when a listener cancelled it.
    bool dispatch(Node* target, const std::string& type, bool bubbles = true, bool cancelable = true);
    // Runs timers that are due. The shell calls this when a wake() delay has passed.
    void pump();
    // Devtools console: evaluates in the page's global scope. Returns false and the error text on failure.
    bool eval(const std::wstring& source, std::wstring& out);
    bool stopped() const;  // scripts were halted (time limit)

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace tb
