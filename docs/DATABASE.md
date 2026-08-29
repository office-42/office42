# The database

office42 can keep a **SQLite** database beside the sheet, or inside the
book itself. SQLite is a whole relational database in one file, with no
server to install and nothing to configure; it is already on nearly
every machine there is, and it is in the public domain.

A spreadsheet and a database want the same thing from each other. The
database is where rows belong when there are more of them than a person
wants to scroll through, or when several books read the same data; the
sheet is where you work them out, chart them and print them. office42
joins the two in the plainest way it can: a query's answer is a
rectangle of values, and a rectangle of values is what a sheet is made
of.

## Where the database lives

**Data ▸ Database ▸ Connect...** opens a database file that is already
on the disk (`.sqlite`, `.sqlite3` or `.db`) and remembers where it is.
The book keeps the path, so opening the book again finds the same
database. Several books can read one database this way, and so can
anything else on the machine.

**Data ▸ Database ▸ Embed New Database** makes an empty database
*inside the book*. While the book is open the database is a file of its
own in the system's temporary directory -- SQLite reads files -- and
when the book is saved as `.gnumeric` the whole database is written
into it, base64 in an element of office42's own. Opening the book
unpacks it again. The book and its data are then one thing you can
send to somebody.

Only `.gnumeric` carries a database. A book saved as `.xlsx` or `.xls`
keeps the values that were laid out on the sheets and forgets where
they came from.

## Getting data into the sheet

**Data ▸ Database ▸ Get Data...** shows the tables the database holds
-- click one and the query that reads it is written for you -- and takes
any query you care to write. The answer is laid out from the active
cell:

| SQLite | in the cell |
|---|---|
| INTEGER, REAL | a number |
| TEXT, BLOB | text, entered as text so part numbers stay part numbers |
| NULL | an empty cell |

Tick *Write the column names above it* and the first row of the answer
is the headings.

The sheet remembers the query and the rectangle its answer filled.
**Data ▸ Database ▸ Refresh Queries** runs every query on the sheet
again, clears what the last answer filled and lays the new one out, so
a book of reports catches up with the database in one step. Formulas
over the answer follow it, as any formula follows the cells it reads.

## Sending data to the database

**Data ▸ Database ▸ Send Selection to Table...** adds the selected rows
to a table. If the table is not there it is made -- from the first
row's headings when you say the selection has them, or as `C1`, `C2`,
`C3` when it has not. Numbers go in as numbers and everything else as
text, with quotes doubled where they need to be.

## A query in a cell

```
=SQLVALUE("SELECT SUM(amount) FROM sales")
=SQLVALUE("SELECT region FROM sales ORDER BY amount DESC", 1, 1)
```

`SQLVALUE(query, column, row)` asks the book's database and answers
with one cell of what it says -- the first by default, which is what a
`SELECT SUM(...)` or a `SELECT COUNT(*)` has to say. `column` and
`row` count from one. It is `#N/A` when the book has no database and
`#VALUE!` when the query will not run.

A whole table does not fit in a cell; that is what Get Data is for. The
connection is opened once and kept, so a sheet full of `SQLVALUE` does
not open the file a hundred times.

## From office42-calc

The terminal front-end has the same thing, which is how all of it was
checked:

| Command | What it does |
|---|---|
| `db FILE` | open (or make) a database file, and remember it on the book |
| `db` | say which database the book has |
| `dbembed` | make a database inside the book |
| `dbtables` | the tables |
| `dbcols TABLE` | the columns of one |
| `dbexec STATEMENT` | run a statement: CREATE, INSERT, UPDATE, DELETE |
| `sql A1 QUERY` | run a query and lay the answer out at A1 |
| `sqlprint QUERY` | run a query and print the answer |
| `dbput TABLE A1:C9 [noheadings]` | send a range to a table |
| `dbrefresh` | run this sheet's queries again |
| `queries` | the queries this sheet remembers |

```
$ office42-calc
db C:/tmp/sales.sqlite
dbexec CREATE TABLE sales (region TEXT, quarter TEXT, amount REAL)
dbexec INSERT INTO sales VALUES ('North','Q1',1200.5),('South','Q1',900)
sql A1 SELECT region, SUM(amount) AS total FROM sales GROUP BY region
D1 = =SUM(B2:B3)
dbrefresh
```

## How it is stored

In the `.gnumeric` file, at the workbook level:

```xml
<gnm:o42-Database Path="C:/data/sales.sqlite"/>
<gnm:o42-Database Embedded="1">U1FMaXRlIGZvcm1hdCAz...</gnm:o42-Database>
```

and at the sheet level, one for each query laid out on it:

```xml
<gnm:o42-Query At="A1:B4" Headings="1">SELECT region, SUM(amount) ...</gnm:o42-Query>
```

Gnumeric passes over elements it does not know, so a book with a
database in it still opens there -- with the values on the sheets, and
without the database.

## Building without it

SQLite is found with `pkg-config` and can be turned off:

```
meson setup builddir -Dsqlite=disabled
```

Without it the Data ▸ Database menu says this build has no SQLite in
it, `SQLVALUE` is not among the functions, and everything else works as
before.

## What is not there

- One database per book. Excel and Gnumeric will hold several
  connections at once; office42 holds one.
- No other database. SQLite is a file, which is what makes it fit
  inside a book; PostgreSQL and the rest need a driver, a host, a user
  and a password, and none of that is here.
- No parameters in a query yet -- no `WHERE region = ?` filled from a
  cell. Write the query with the value in it, or build it with a
  formula and pass it to `SQLVALUE`.
- Refresh is something you ask for. Nothing watches the database and
  nothing polls it.
