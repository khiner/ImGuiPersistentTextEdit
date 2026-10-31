#include "Cursors.h"

#include "Layout.h"

#include <algorithm>

namespace ipte {
void SetCursor(Cursors &c, DocPos start, DocPos end) {
    c.Items.clear();
    c.Items.push_back({start, end, true, true});
    c.Main = c.Current = 0;
}

void AddCursor(Cursors &c, DocPos start, DocPos end) {
    c.Items[c.Current].Current = false;
    c.Items.push_back({start, end, false, true});
    c.Current = uint32_t(c.Items.size() - 1);
}

bool AnyHasSelection(const Cursors &c) { return std::ranges::any_of(c.Items, &Cursor::HasSelection); }
bool HasMultiple(const Cursors &c) { return c.Items.size() > 1; }

void ClearAdditional(Cursors &c, bool reset) {
    std::erase_if(c.Items, [](const Cursor &x) { return !x.Main; });
    c.Main = c.Current = 0;
    c.Items.front().Current = true;
    if (reset) ResetToEnd(c.Items.front());
}

void Grow(Cursor &c, DocPos pos) {
    if (pos < c.SelectionStart()) Update(c, c.SelectionEnd(), pos);
    else if (pos > c.SelectionEnd()) Update(c, c.SelectionStart(), pos);
}

void UpdateCursors(State &s) {
    auto &c = s.Cursors;
    for (auto &x : c.Items) {
        for (auto *pos : {&x.Start, &x.End}) {
            if (!IsHidden(s, pos->Line)) continue;
            *pos = LeftBeforeHiddenLines(s, *pos);
            x.Updated = true;
        }
    }
    if (std::ranges::none_of(c.Items, &Cursor::Updated)) return;
    for (auto &x : c.Items) x.Updated = false;
    if (!HasMultiple(c)) return;
    auto &items = c.Items;
    std::ranges::sort(items, {}, &Cursor::SelectionStart);
    // Merge from the back, as upstream does.
    for (size_t i = items.size() - 1; i > 0; --i) {
        const auto &cursor = items[i];
        auto &previous = items[i - 1];
        if (previous.SelectionEnd() < cursor.SelectionEnd()) {
            if (previous.SelectionEnd() <= cursor.SelectionStart()) continue;
            if (cursor.End < cursor.Start) Update(previous, cursor.SelectionEnd(), previous.SelectionStart());
            else Update(previous, previous.SelectionStart(), cursor.SelectionEnd());
        }
        if (cursor.Main) previous.Main = true;
        if (cursor.Current) previous.Current = true;
        items.erase(items.begin() + int64_t(i));
    }
    c.Main = c.Current = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].Main) c.Main = uint32_t(i);
        else if (items[i].Current) c.Current = uint32_t(i);
    }
}

void AdjustForInsert(Cursors &c, size_t from, DocPos insertStart, DocPos insertEnd, bool includeCurrent) {
    for (auto i = includeCurrent ? from : from + 1; i < c.Items.size(); ++i) {
        for (auto *pos : {&c.Items[i].Start, &c.Items[i].End}) {
            if (pos->Line == insertStart.Line) pos->Index += insertEnd.Index - insertStart.Index;
            pos->Line += insertEnd.Line - insertStart.Line;
        }
    }
}

void AdjustForDelete(Cursors &c, size_t from, DocPos deleteStart, DocPos deleteEnd, bool includeCurrent) {
    for (auto i = includeCurrent ? from : from + 1; i < c.Items.size(); ++i) {
        for (auto *pos : {&c.Items[i].Start, &c.Items[i].End}) {
            if (deleteStart.Line == deleteEnd.Line) {
                if (pos->Line == deleteEnd.Line) pos->Index -= deleteEnd.Index - deleteStart.Index;
            } else {
                pos->Line -= deleteEnd.Line - deleteStart.Line;
                if (pos->Line == deleteEnd.Line) pos->Index -= deleteEnd.Index;
            }
        }
    }
}
} // namespace ipte
