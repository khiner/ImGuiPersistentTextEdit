#pragma once

#include "TextEdit.h"

namespace ipte {
void SetCursor(Cursors &c, DocPos start, DocPos end);
inline void SetCursor(Cursors &c, DocPos pos) { SetCursor(c, pos, pos); }
void AddCursor(Cursors &c, DocPos start, DocPos end);
inline void AddCursor(Cursors &c, DocPos pos) { AddCursor(c, pos, pos); }
bool AnyHasSelection(const Cursors &c);
bool AnyHasUpdate(const Cursors &c);
bool HasMultiple(const Cursors &c);
void ClearAll(Cursors &c);
void ClearAdditional(Cursors &c, bool reset = false);
// Moves cursors off hidden lines. If any cursor was updated, sorts and merges overlapping cursors.
void UpdateCursors(State &s);
// Shifts cursors after index `from`, or from it with `includeCurrent`, for an insert or delete.
void AdjustForInsert(Cursors &c, size_t from, DocPos insertStart, DocPos insertEnd, bool includeCurrent = false);
void AdjustForDelete(Cursors &c, size_t from, DocPos deleteStart, DocPos deleteEnd, bool includeCurrent = false);

inline Cursor &CurrentCursor(Cursors &c) { return c.Items[c.Current]; }
inline const Cursor &CurrentCursor(const Cursors &c) { return c.Items[c.Current]; }
inline Cursor &MainCursor(Cursors &c) { return c.Items[c.Main]; }
inline const Cursor &MainCursor(const Cursors &c) { return c.Items[c.Main]; }

inline void Update(Cursor &c, DocPos end) {
    c.End = end;
    c.Updated = true;
}
inline void Update(Cursor &c, DocPos start, DocPos end) {
    c.Start = start;
    c.End = end;
    c.Updated = true;
}
inline void Update(Cursor &c, DocPos pos, bool keep) {
    if (keep) Update(c, pos);
    else Update(c, pos, pos);
}
void Grow(Cursor &c, DocPos pos);
inline void ResetToStart(Cursor &c) { Update(c, c.SelectionStart(), c.SelectionStart()); }
inline void ResetToEnd(Cursor &c) { Update(c, c.SelectionEnd(), c.SelectionEnd()); }
} // namespace ipte
