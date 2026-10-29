# Upstream bugs

Bugs in [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit) that this editor reproduces to keep the draw-list test exact. Each is a candidate for an upstream pull request.

| Where | Actual | Expected |
| --- | --- | --- |
| `render`, first frame | `PushStyleColor(ChildBg, palette.get(background))` reads `palette` before `updatePalettes` has ever filled it, so the first frame's background is whatever memory held. | Shade the palette before the first use, or initialize `palette` from `paletteBase` in `SetPalette`. |
| `renderCurrentLineHighlight` | The highlight rectangle's left edge is `textLeftOffset` with no window position added, so it is only correct when the editor sits at x = 0. | Add `cursorScreenPos.x`, as every other text-area rectangle does. |
| `handleCharacter`, wrapping a selection in a pair | `cursors.adjustForInsert(cursor, start, end1)` is passed the selection start as the insert start, so cursors after it on the line shift by the selection's length plus one. | Pass the insert position, `end`, so later cursors shift by the one glyph inserted. |
| `LineFold::foldAroundLine` | Finds the enclosing foldable line as `lineToFold`, then folds `line`, the argument, instead. Nothing folds unless the argument is itself a fold start. | Set `document[lineToFold].foldingState`. |
| `Document::findNextNonWhiteSpace` with `includeEndOfLine` | On the last line it compares `from.line` to `size()`, which no line equals, and `getRightAfterHiddenLines` returns the same position, so a last line of trailing whitespace loops forever. | Compare to `size() - 1`. |
| `TypeSetter::docPos2VisPos` on a hidden line | Reads `at(line.row)`, where a hidden line's row is the next visible row, not the folded line's. For a fold at the end of the document that row does not exist and `at` throws. | Use the folded line's last row. |
| `SetLineBreakConfig` | Stores the new rules but marks no line for re-typesetting, so lines keep the breaks of the old rules until they are edited. Not reproduced here: every line re-wraps. | Mark every line for re-typesetting when the configuration changes. |
