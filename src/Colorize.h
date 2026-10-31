#pragma once

#include "TextEdit.h"

namespace ipte {
// Updates `s.Derived` if its inputs changed. Returns whether the bracket pairs changed.
bool Refresh(State &s);
Color ColorAt(const State &s, uint64_t offset);
bool LineInComment(const State &s, uint64_t line);
bool LineInString(const State &s, uint64_t line);
// The innermost pair around the position(s), or null.
const Bracket *EnclosingBrackets(const View &v, DocPos pos);
const Bracket *EnclosingBrackets(const View &v, DocPos first, DocPos last);
const Bracket *InnerBrackets(const View &v, DocPos first, DocPos last);
} // namespace ipte
