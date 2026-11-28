#pragma once
#include "common.h"

namespace tb {

struct Style;

// DOM node. Elements, text and comments share one struct to keep the tree compact.
struct Node {
    enum class T : uint8_t { Document, Element, Text, Comment };
    T type = T::Element;
    std::string name;  // lowercase tag name
    std::string text;  // text / comment data
    std::string id;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::string> classes;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> kids;
    // A template element's contents (HTML 4.12.3): a document fragment of their own, which is not part of the
    // page. What is written between <template> and </template> goes here, not among the element's children.
    std::unique_ptr<Node> content;
    // An element's shadow root (DOM 4.8): a fragment named "#shadow-root" whose parent is this element, its host.
    // It is not among the children either; the flat tree (FlatKids below) is where the two come together.
    std::unique_ptr<Node> shadow;
    std::unique_ptr<Style> style;  // computed style (elements only)
    // The ::before and ::after boxes of an element (CSS 2 12.1), made when its style is computed: elements named
    // "::before" and "::after" holding the generated text. They are not part of the DOM; FlatKids lists them first and last.
    std::unique_ptr<Node> before, after;
    uint32_t elemIndex = 0, elemCount = 1;
    // Border box from the last layout (document coordinates), used by devtools.
    float bx = 0, by = 0, bw = 0, bh = 0;
    bool hasBox = false;
    int16_t layer = 0;  // the paint layer of that box (DItem::layer): of two boxes at a point, the higher one is on top
    // A scroll container (CSS Overflow 3): how far its content is scrolled down, and how far it can be. Layout sets
    // the range and holds the offset within it; scripts and the mouse wheel change the offset.
    float scrollTop = 0, scrollRange = 0;
    uint8_t ceState = 0;  // custom element state (HTML 4.13.3): 0 not custom, or not yet; 1 custom; 2 its constructor failed

    Node();
    ~Node();
    const std::string* attr(std::string_view n) const;
    bool is(std::string_view n) const { return type == T::Element && name == n; }
    bool isShadowRoot() const { return name == "#shadow-root"; }
    Node* append(std::unique_ptr<Node> n) {
        n->parent = this;
        kids.push_back(std::move(n));
        return kids.back().get();
    }
};

struct Document {
    std::unique_ptr<Node> root;
    std::string url, title, baseUrl;
    Node* html = nullptr;
    Node* head = nullptr;
    Node* body = nullptr;
    size_t nodeCount = 0;
    bool quirks = false;  // no doctype: legacy rendering quirks apply
};

std::string text_content(const Node* n);

// The children an element is styled, laid out and painted with (the flat tree, CSS Scoping 3.1). Without shadow
// roots these are its own children. A host's are those of its shadow root, and inside a shadow tree every <slot>
// stands for the host's children assigned to it, or for its own when none are.
class FlatKids {
public:
    explicit FlatKids(const Node* n);
    size_t size() const { return plain_ ? plain_->size() : mixed_.size(); }
    Node* operator[](size_t i) const { return plain_ ? (*plain_)[i].get() : mixed_[i]; }
    struct It {
        const FlatKids* f;
        size_t i;
        Node* operator*() const { return (*f)[i]; }
        It& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const It& o) const { return i != o.i; }
    };
    It begin() const { return {this, 0}; }
    It end() const { return {this, size()}; }

private:
    const std::vector<std::unique_ptr<Node>>* plain_ = nullptr;
    std::vector<Node*> mixed_;
};
Node* shadow_root_of(const Node* n);            // the shadow root whose tree holds n, or null
Node* assigned_slot(const Node* n);             // the <slot> a child of a shadow host is assigned to, or null
Node* flat_parent(const Node* n);               // the parent in the flat tree: the slot of an assigned node, a shadow root's host
void assigned_nodes(const Node* slot, std::vector<Node*>& out);  // the host's children assigned to this slot
extern int g_shadowRoots;  // shadow roots made so far in this process: while 0, every flat tree is the plain tree

}  // namespace tb
