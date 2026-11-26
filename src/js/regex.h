// Regular expression engine: compiles a JavaScript pattern to a small program and runs it with backtracking.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tb::js {

struct RegexProgram;

struct RegexFlags {
    bool global = false, ignoreCase = false, multiline = false, dotAll = false, unicode = false, sticky = false, hasIndices = false;
};

// Returns null and fills `error` when the pattern is not valid.
std::shared_ptr<RegexProgram> regexCompile(const std::u16string& pattern, const RegexFlags& flags, std::string& error);

int regexGroupCount(const RegexProgram& p);  // capture groups, including group 0 (the whole match)
const std::vector<std::pair<std::u16string, int>>& regexGroupNames(const RegexProgram& p);

// Looks for a match starting at `from` or later (exactly at `from` when sticky). On success caps holds a
// start,end pair of offsets for every group, -1 for groups that took no part in the match.
bool regexExec(const RegexProgram& p, const std::u16string& s, size_t from, bool sticky, std::vector<int>& caps);

// Checks a regular expression literal without keeping the result; false with `error` set when it is not valid.
bool regexValidate(const std::u16string& pattern, const std::u16string& flags, std::string& error);

// Unicode ID_Start and ID_Continue for code units outside ASCII (identifiers, ECMA-262 12.7).
bool unicodeIdStart(uint32_t cp);
bool unicodeIdContinue(uint32_t cp);

}  // namespace tb::js
