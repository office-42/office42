# Sample books

Two small books in Excel's two formats, for opening by hand and for
checking that office42 reads what other programs write.

| File | What is in it |
|---|---|
| `formats.xlsx`, `formats.xls` | number formats (grouped, currency, percent, scientific, negative, date, boolean), alignment, italic and underline, text turned 45 degrees, wrapped text, a merged title, colours, borders, column widths and a tall row |
| `formulas.xlsx`, `formulas.xls` | a ten-row table, and fifty formulas over it -- the aggregates, the criteria functions, lookup, text, dates, money, statistics and an array formula -- each beside its own text |
| `*.fods` | the flat OpenDocument each was written from |

## Where they come from

The content is invented for this project; the numbers are made up. The
`.fods` files are written by
[`make-samples.py`](make-samples.py), and **LibreOffice**
converts each into `.xlsx` and `.xls`. That is the point of the
exercise: the Excel-format files are written by a program that is not
office42, so reading them tests the reader rather than the writer.

Nothing here is anyone else's document. See [NOTICE.md](../NOTICE.md).

## Using them

```sh
office42 samples/formats.xlsx          # look at it
office42-calc                          # or read it in the terminal
  load samples/formulas.xlsx
  dump
  C45
```

`formulas.xlsx` carries the value LibreOffice worked out for every
formula, cached in the file as `<v>`. Comparing those with what
office42 computes is the check that matters, and the last time it was
run the only differences left were these:

| Cell | Formula | Why they differ |
|---|---|---|
| `C59` | `chidist(3.84,1)` | LibreOffice will not take the lower-case spelling in a `.xlsx`; office42 will |
| `C60` | `SUM(D2:D11*E2:E11)` | an array formula: office42 spills it as Excel 365 does, LibreOffice wants Ctrl+Shift+Enter |
| `C63` | `SUM(D2:INDEX(D2:D11,5))` | a range whose far end is a function. Excel and LibreOffice allow it; office42 says `#REF!` -- the one thing in these books it cannot yet do |

Everything else agrees to the last digit, currency and percent cells
included, and the formats book shows what it should down to the
`[$kr-414]` currency LibreOffice put on three of the money cells.
