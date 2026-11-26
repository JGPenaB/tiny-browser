#pragma once
#include "dom.h"

namespace tb {

// Parses an HTML document (UTF-8) into a DOM tree. Implements the HTML5 tokenizer
// states that matter in practice and a simplified tree builder with implied tags.
std::unique_ptr<Document> parse_html(std::string_view src, const std::string& url);
void decode_entities(std::string_view s, std::string& out);

}  // namespace tb
