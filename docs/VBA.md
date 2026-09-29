# Visual Basic for Applications

A book from Excel with macros in it -- an `.xlsm`, or an `.xls` that
has them -- opens with its Visual Basic project, which office42 shows
in an editor in the shape of Excel 97's, runs with an interpreter of
its own, and writes back so that Excel opens it again with its macros.
Python stays office42's own macro language (see [PYTHON.md](PYTHON.md));
this is for the macros the world already has.

## Opening a book with macros

Nothing in the project runs by itself.  The bar under the formula bar
says that the book has macros and that they are disabled; **Enable
Macros** makes the book's functions usable from cells, lets its event
procedures hear what they listen for, and runs `Workbook_Open` and
then `Auto_Open`, as Excel does on opening a trusted book.  A book
with Python scripts as well offers **Enable Content**, which does both.

A project locked for viewing opens in the editor all the same: the lock
belongs to Excel's editor, and the text is in the file for anyone to
read.  The password is carried over for Excel.

## Running macros

- **Tools ▸ Macro ▸ Macros… (Alt+F8)** lists every public `Sub` that
  takes no arguments, with the description Excel's Macro Options gave
  it, beside the book's Python scripts.  **Run**, **Edit** (the editor,
  at the `Sub`), **Delete** (the `Sub` goes from its module) and
  **Options** (the shortcut key and the description) work on them.
- **A shortcut key** from Macro Options runs the macro: a lower-case
  letter with Ctrl, an upper-case one with Ctrl+Shift.
- **A button**, or any form control, whose macro is a Visual Basic one
  runs it when clicked.
- **A function** in a standard module is a worksheet function once the
  macros are enabled: `=AddTax(A1)` or `=CountBig(A1:A9, 60)` in a
  cell.  A `Range` argument arrives as a `Range`, `Application.Caller`
  is the cell being calculated, and an error inside gives `#VALUE!`.
- **Events**: `Workbook_Open`, `Workbook_BeforeClose`,
  `Workbook_BeforeSave`, `Workbook_SheetChange`,
  `Workbook_SheetSelectionChange` in ThisWorkbook; `Worksheet_Change`
  and `Worksheet_SelectionChange` in a sheet's module; `Auto_Open` and
  `Auto_Close` in a standard module.  `Application.EnableEvents = False`
  silences them.  `Cancel = True` in `BeforeSave` leaves the book
  unsaved, and in `BeforeClose` keeps it open.  What an event procedure
  writes with `Debug.Print` goes to the Immediate window, and only a
  run-time error in one puts up a box.

A run-time error stops the macro with Excel's box -- `Run-time error
'9': Subscript out of range` -- and the module, procedure and line it
happened on; with the editor open the caret goes to that line.
`MsgBox` asks with the buttons it is given and returns which was
pressed; `InputBox` and `Application.InputBox` ask for a line.

**Stopping a macro.** A macro that has run for a second gets a small
window of its own, and meanwhile the grid and the status bar show what
it has done so far (unless `Application.ScreenUpdating` is False).
**Esc** or **Ctrl+Break** interrupts it and asks, as Excel does, "Code
execution has been interrupted": **Continue** goes on, **End** stops
it.  With `Application.EnableCancelKey = xlErrorHandler` the key is
error 18 instead, for the macro's own `On Error` handler, and with
`xlDisabled` it does nothing.

## The Visual Basic Editor

**Tools ▸ Macro ▸ Visual Basic Editor (Alt+F11).**  The project's
modules are on the left in Excel's folders: Microsoft Excel Objects
(ThisWorkbook, and each sheet's module under its code name with the
tab's name beside it), Forms, Modules and Class Modules.  The module's
code is in the middle, in Visual Basic's colours; the Attribute lines
Excel keeps out of sight stay out of sight here, and are put back when
the text is saved.  Under it is the Immediate window: `?expression`
prints a value, anything else is run as statements, and `Debug.Print`
writes there.

| Key | |
|---|---|
| F5 | run the `Sub` the caret is in |
| F8 | step into it, a line at a time, with its variables shown beside the code |
| F9 | set or clear a breakpoint on the caret's line |
| Ctrl+G | the Immediate window |
| Ctrl+S | save the book |
| Alt+F11 | back to the book |

**Insert Module** and **Insert Class** add modules; **Import File…**
reads a `.bas` or `.cls` exported from Excel, and **Export File…**
writes one Excel can import; **Remove** takes a module out; the name
field renames a module or a class.  **Compile** checks every module and
puts the caret on the first line it cannot make sense of.

## Saving

| Saved as | The project |
|---|---|
| `.xlsm` | Kept as it came while it is untouched.  Once edited, `vbaProject.bin` is built afresh from the modules' text -- a project with the source and no compiled code, which the format allows and Excel compiles on opening.  The references, the project's settings and password and any forms' storages are carried over; a digital signature is dropped, since it no longer matches. |
| `.xls` | The same, as the `_VBA_PROJECT_CUR` storage, with the code names in their records. |
| `.gnumeric` | The modules' text and the original project, in office42's own elements, which Gnumeric passes over. |
| `.xlsx`, `.ods` | Left out, as Excel leaves it out of an `.xlsx`; saving says so. |

A book that gets its first module in office42 is given what Excel gives
every book -- a ThisWorkbook module and one for each sheet, with the
code names that tie them together; it wants saving as `.xlsm` to keep
them.

## The language

Visual Basic 6's, as VBA has it:

- **Declarations**: `Dim`, `Private`, `Public`, `Static`, `Const`,
  `Type … End Type`, `Enum`, `Option Explicit`, `Option Base`,
  `Option Compare Text`, `Declare` (see below), and the type
  suffixes `% & ! # @ $`.
- **Types**: `Variant` with `Empty`, `Null`, `Nothing` and `Missing`,
  `Boolean`, `Byte`, `Integer`, `Long`, `LongLong`, `Single`, `Double`,
  `Currency`, `Date`, `String` and `String * n`, objects, user-defined
  types, arrays fixed and dynamic with any bounds (`ReDim Preserve`,
  `Erase`), the overflow and type-mismatch errors of each.
- **Procedures**: `Sub`, `Function`, `Property Get/Let/Set`, `ByRef` by
  default and `ByVal`, `Optional` with defaults and `IsMissing`,
  `ParamArray`, named arguments (`Destination:=`), `Call`, `Exit`,
  recursion.
- **Flow**: `If` in both forms, `Select Case` with `Is`, `To` and
  lists, `For … Next` (and `Next j, i`), `For Each`, `Do … Loop` in its
  four forms, `While … Wend`, `With`, `GoTo`, `GoSub … Return`,
  `On … GoTo`, `End`, `Stop`.
- **Errors**: `On Error GoTo`, `On Error Resume Next`, `On Error GoTo 0`,
  `Resume`, `Resume Next`, `Resume label`, the `Err` object with
  `Raise` and `Clear`, and Excel's numbers and texts for the errors.
- **Classes**: class modules with `New`, `Set`, `Is`, `TypeOf … Is`,
  `Me`, `Class_Initialize` and `Class_Terminate`, `Implements`,
  `Event`, `RaiseEvent` and `WithEvents`, a default member; `Collection`.
- **The library**: the string, number, conversion, date, `Format` and
  `Is…` functions, `Split`, `Join`, `Filter`, `Array`, `IIf`,
  `Choose`, `Switch`, `CallByName`, `Like`, `Environ`, `SaveSetting`
  and its kin, `Rnd` and `Randomize`; the file statements
  (`Open`, `Print #`, `Write #`, `Input #`, `Line Input #`, `Get`, `Put`,
  `Close`, `Seek`, `EOF`, `LOF`, `FreeFile`) and `Dir`, `Kill`,
  `MkDir`, `FileCopy`, `FileLen`, `Name … As`.
- **`CreateObject`**: `Scripting.Dictionary`,
  `Scripting.FileSystemObject` (with `TextStream`, `File` and `Folder`),
  `VBScript.RegExp`, and `Excel.Application` for itself.
- **`Declare`**: `Sleep` and `GetTickCount`, which is what most macros
  declare; any other call into a Windows DLL is error 453.

## Excel's objects

`Application` and its global names (`ActiveSheet`, `ActiveCell`,
`Selection`, `Range`, `Cells`, `Worksheets`, `Sheets`, `ThisWorkbook`,
`ActiveWorkbook`, `Names`, `Evaluate` and `[A1]`, `WorksheetFunction`
with every worksheet function, `Application.VLookup` and its kin that
return an error value rather than raising one, `StatusBar`,
`ScreenUpdating`, `EnableEvents`, `DisplayAlerts`, `Calculation`,
`Intersect`, `Union`, `InputBox`, `Transpose`, `ConvertFormula`, `Run`,
`Wait`); `Workbook` with `Save`, `SaveAs`, `Close`, `Worksheets`,
`Sheets`, `Names`, `BuiltinDocumentProperties`; `Worksheet` with
`Cells`, `Range`, `Rows`, `Columns`, `UsedRange`, `Shapes`, `Name`,
`Index`, `Visible`, `Activate`, `Add`, `Copy`, `Move`, `Delete`,
`Protect`, `Unprotect`, `PageSetup`, `Sort`, `Calculate`; and `Range`
with most of what macros use: `Value`, `Value2`, `Formula`,
`FormulaR1C1`, `Text`, `NumberFormat`, `Address` in both styles,
`Offset`, `Resize`, `End`, `CurrentRegion`, `EntireRow`,
`EntireColumn`, `SpecialCells`, `Find` and `FindNext`, `Replace`,
`Copy` with a destination, `PasteSpecial`, `Insert`, `Delete`,
`Clear` and its kin, `Sort`, `AutoFilter`, `AutoFit`, `Merge`,
`Font`, `Interior`, `Borders`, `HorizontalAlignment`, `WrapText`,
`ColumnWidth`, `RowHeight`, `Hidden`, `Comment` and `AddComment`,
`Hyperlinks`, `Validation`, `FormatConditions`, `Areas`, iteration
with `For Each`, and reading and writing whole arrays at once.

## Not there

- **UserForms.** A form's module and its storage are kept and saved
  back, but the form is not shown; a macro that shows one stops with an
  error.
- **ActiveX controls** on a sheet, and the Windows API beyond `Sleep`
  and `GetTickCount`.
- **Other books.** `Workbooks.Open` and `Workbooks.Add` are errors: a
  macro works on the book it is in, and office42 opens other books in
  windows of their own.  `Application.OnTime` is an error too.
- **Charts** from Visual Basic: `ChartObjects` finds a sheet's charts
  to move, size and delete, but the chart inside one -- its type, its
  series, its titles -- is not in the object model yet.
- **`Shell`, `SendKeys` and `AppActivate`**: a macro here does not start
  programs or type into them.
- **The compiled code.** office42 reads the source Excel keeps beside
  its p-code and runs that; a file whose source was taken out and only
  the p-code left is one it cannot run.

## From the terminal

`office42-calc` has the project too, which is how all of this is
checked:

```
load Book.xlsm
vba                    the modules and the macros
vbacode Module1        a module's text
vbaimport Tools.bas    a module from a file; vbaexport Module1 out.bas
vbadel Module1         a module removed
vbarun Main            a macro run; Debug.Print writes to stdout
vbaexec ?Range("A1").Value
                       a line as the Immediate window runs it
vbaenable              Enable Macros: functions and events live
vbacheck               every module compiled
```

## How it is built

The project is read and written in C (`src/io/o42-vba.c` over the
compound-file code in `o42-ole2.c`), after [MS-OVBA]: the `dir` stream,
the compressed module streams and the PROJECT stream, and a project of
source alone written back.  The interpreter is Python, embedded as the
rest of the script layer is: `src/script/vba.py` is the language --
lexer, parser, and a compiler that turns each procedure into closures
-- and `vbaexcel.py` is Excel's object model over the `_office42`
module, which is the only way it reaches the book.  Neither sees GTK.
The editor is `src/ui/o42-vbe.c`.  Without Python the project is still
read, shown, edited and saved; it is only not run.
