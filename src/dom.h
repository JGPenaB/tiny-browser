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
    std::unique_ptr<Style> style;  // computed style (elements only)
    uint32_t elemIndex = 0, elemCount = 1;
    // Border box from the last layout (document coordinates), used by devtools.
    float bx = 0, by = 0, bw = 0, bh = 0;
    bool hasBox = false;

    Node();
    ~Node();
    const std::string* attr(std::string_view n) const;
    bool is(std::string_view n) const { return type == T::Element && name == n; }
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

}  // namespace tb
