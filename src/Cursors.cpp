#include "Cursors.h"

#include "Document.h"
#include "Layout.h"

#include <algorithm>

namespace ipte {
namespace {
void Reset(Cursors &c) {
    c.Items.clear();
    c.Main = c.Current = 0;
}

DocPos AdjustedForInsert(DocPos pos, DocPos insertStart, DocPos insertEnd) {
    if (pos.Line == insertStart.Line) pos.Index += insertEnd.Index - insertStart.Index;
    pos.Line += insertEnd.Line - insertStart.Line;
    return pos;
}

DocPos AdjustedForDelete(DocPos pos, DocPos deleteStart, DocPos deleteEnd) {
    if (deleteStart.Line == deleteEnd.Line) {
        if (pos.Line == deleteEnd.Line) pos.Index -= deleteEnd.Index - deleteStart.Index;
    } else {
        pos.Line -= deleteEnd.Line - deleteStart.Line;
        if (pos.Line == deleteEnd.Line) pos.Index -= deleteEnd.Index;
    }
    return pos;
}
} // namespace

void SetCursor(Cursors &c, DocPos start, DocPos end) {
    Reset(c);
    c.Items.push_back({start, end, true, true});
}

void AddCursor(Cursors &c, DocPos start, DocPos end) {
    c.Items[c.Current].Current = false;
    c.Items.push_back({start, end, false, true});
    c.Current = uint32_t(c.Items.size() - 1);
}

bool AnyHasSelection(const Cursors &c) {
    return std::ranges::any_of(c.Items, [](const Cursor &x) { return x.HasSelection(); });
}
bool AnyHasUpdate(const Cursors &c) {
    return std::ranges::any_of(c.Items, [](const Cursor &x) { return x.Updated; });
}
bool HasMultiple(const Cursors &c) { return c.Items.size() > 1; }

void ClearAll(Cursors &c) {
    Reset(c);
    c.Items.push_back({{}, {}, true, true});
}

void ClearAdditional(Cursors &c, bool reset) {
    std::erase_if(c.Items, [](const Cursor &x) { return !x.Main; });
    c.Main = c.Current = 0;
    c.Items.front().Current = true;
    if (reset) ResetToEnd(c.Items.front());
}

void Grow(Cursor &c, DocPos pos) {
    if (pos < c.SelectionStart()) {
        if (c.End > c.Start) std::swap(c.End, c.Start);
        c.End = pos;
        c.Updated = true;
    } else if (pos > c.SelectionEnd()) {
        if (c.End < c.Start) std::swap(c.End, c.Start);
        c.End = pos;
        c.Updated = true;
    }
}

void UpdateCursors(State &s) {
    auto &c = s.Cursors;
    for (auto &x : c.Items) {
        for (auto *pos : {&x.Start, &x.End}) {
            if (!IsHidden(s, pos->Line)) continue;
            auto line = pos->Line - 1;
            while (IsHidden(s, line)) --line;
            *pos = {line, LineLength(s.Doc, line)};
            x.Updated = true;
        }
    }
    if (!AnyHasUpdate(c)) return;
    for (auto &x : c.Items) x.Updated = false;
    if (!HasMultiple(c)) return;
    auto &items = c.Items;
    std::ranges::sort(items, [](const Cursor &a, const Cursor &b) { return a.SelectionStart() < b.SelectionStart(); });
    // Merge from the back, as upstream does.
    for (size_t i = items.size() - 1; i > 0;) {
        auto const &cursor = items[i];
        auto &previous = items[i - 1];
        if (previous.SelectionEnd() >= cursor.SelectionEnd()) {
            if (cursor.Main) previous.Main = true;
            if (cursor.Current) previous.Current = true;
            items.erase(items.begin() + int64_t(i));
            --i;
        } else if (previous.SelectionEnd() > cursor.SelectionStart()) {
            if (cursor.End < cursor.Start) Update(previous, cursor.SelectionEnd(), previous.SelectionStart());
            else Update(previous, previous.SelectionStart(), cursor.SelectionEnd());
            if (cursor.Main) previous.Main = true;
            if (cursor.Current) previous.Current = true;
            items.erase(items.begin() + int64_t(i));
            --i;
        } else {
            --i;
        }
    }
    c.Main = c.Current = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].Main) c.Main = uint32_t(i);
        else if (items[i].Current) c.Current = uint32_t(i);
    }
}

void AdjustForInsert(Cursors &c, size_t from, DocPos insertStart, DocPos insertEnd, bool includeCurrent) {
    for (auto i = includeCurrent ? from : from + 1; i < c.Items.size(); ++i) {
        auto &x = c.Items[i];
        x.Start = AdjustedForInsert(x.Start, insertStart, insertEnd);
        x.End = AdjustedForInsert(x.End, insertStart, insertEnd);
    }
}

void AdjustForDelete(Cursors &c, size_t from, DocPos deleteStart, DocPos deleteEnd, bool includeCurrent) {
    for (auto i = includeCurrent ? from : from + 1; i < c.Items.size(); ++i) {
        auto &x = c.Items[i];
        x.Start = AdjustedForDelete(x.Start, deleteStart, deleteEnd);
        x.End = AdjustedForDelete(x.End, deleteStart, deleteEnd);
    }
}
} // namespace ipte
