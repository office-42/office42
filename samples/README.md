# Sample books

Books to open by hand and to test against: two small ones in Excel's two
formats, which check that office42 reads what other programs write, and
two of its own, which check that the engine still works out what it
worked out before.

| File | What is in it |
|---|---|
| `formats.xlsx`, `formats.xls` | number formats (grouped, currency, percent, scientific, negative, date, boolean), alignment, italic and underline, text turned 45 degrees, wrapped text, a merged title, colours, borders, column widths and a tall row |
| `formulas.xlsx`, `formulas.xls` | a ten-row table, and fifty formulas over it -- the aggregates, the criteria functions, lookup, text, dates, money, statistics and an array formula -- each beside its own text |
| `*.fods` | the flat OpenDocument each was written from |
| `accounts.gnumeric` | a year of double-entry accounts over seven sheets, with four charts: the book on the README's screenshot |
| `income-statement.gnumeric` | a one-sheet income statement, actual against budget, with two charts |

## Where they come from

The content is invented for this project; the numbers are made up, the
company does not exist, and no figure here was taken from anyone's
books. The `.fods` files are written by
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

Everything else agrees to the last digit, currency and percent cells
included, and the formats book shows what it should down to the
`[$kr-414]` currency LibreOffice put on three of the money cells.

## The two books office42 writes itself

`accounts.txt` and `income-statement.txt` are office42-calc scripts, and
the `.gnumeric` beside each is the book the script makes. The comment at
the top of each says how to rebuild it, and how the README's picture is
taken.

`accounts.gnumeric` is a set of books for a small trading company over a
year, and it is meant to be complex enough to be worth testing against:

| Sheet | What is on it |
|---|---|
| Dashboard | four key figures in merged cells, a quarterly table, an operating margin row under a conditional format, and a column chart and a pie chart |
| Profit | the profit and loss by quarter against the budget, with sections, subtotals, indents, variance and a bar chart of the year against the budget |
| Balance | the balance sheet, this year beside last, ending in a line that says whether it balances |
| Cashflow | the cash flow worked back from the profit and the balances, with a line chart of the cash quarter by quarter |
| Trial | the trial balance: every account's opening balance, its debits and credits from the journal, and what it closes at |
| Journal | fifty-one transactions as a table with a filter and a validation rule, each one debit against one credit |
| Accounts | the chart of accounts and what each held on 1 January |

Nothing on it is typed twice: the journal holds the transactions, the
trial balance gathers them with `SUMIF`, and the three statements are
drawn from that with `SUMIFS`, `INDEX` and `MATCH` across the sheets.
So the book is a test as much as a sample -- four sheets end in a line
that says, in words, whether the sums still tie up:

```sh
office42-calc
  load samples/accounts.gnumeric
  sheet Trial
  A27                # The books balance: every debit has its credit.
  sheet Balance
  A24                # Assets match liabilities and equity, to the penny.
  sheet Cashflow
  A24                # The closing cash is the cash in the trial balance.
```

If a change to the engine breaks one of them, those lines say so in
English rather than leaving a wrong number to be spotted.
