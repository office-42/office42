# Office42 in the Microsoft Store

The Store takes desktop programs in two shapes: an MSIX package, or an
MSI/EXE installer left as it is. Office42 goes as MSIX, because the
Store signs an MSIX itself once it has passed certification -- no
code-signing certificate to buy -- and because Windows then installs
and removes it cleanly, with no registry keys or uninstall entries of
the program's own. The procedure is word42's, and this page is its
counterpart of word42's `docs/WINDOWS-STORE.md`.

## Building the package

From an MSYS2 **MINGW64** shell, with the build already done:

```sh
bash build-aux/bundle-windows.sh builddir dist
bash build-aux/pack-msix.sh builddir dist msix
```

The result is `msix/office42-<version>-win64.msix`, unsigned, about
52 MB. The Windows CI job makes the same package on every push and
keeps it as the `office42-windows-msix` artifact.

`bundle-windows.sh` gathers a tree that runs without MSYS2:
`bin/office42.exe` and `bin/office42-calc.exe` with every DLL `ldd`
finds under the prefix, followed until nothing new turns up; the
gdk-pixbuf loaders (the SVG one is what draws the toolbar); Python's
standard library in `lib/python3.X`, where the interpreter looks for it
beside its DLL, without the tests, the IDE and site-packages; Hunspell's
English dictionaries in `share/hunspell`, which the speller looks for
beside its `bin`; poppler's encoding tables; GTK's schemas and icons;
and the message catalogues. The bundle was checked by running it with
nothing but `C:\Windows` on the `PATH`: the window with its icons, a
Python line, an embedded database and a spelling pass.

`pack-msix.sh` then stages that bundle, renders the Store's tiles from
`data/icons/scalable/apps/net.office42.office42.svg` with
`rsvg-convert` -- StoreLogo, Square44x44, Square71x71, Square150x150,
Square310x310 and Wide310x150 at scale 100, 125, 150, 200 and 400, plus
the target-size 16/24/32/48/256 variants of the 44x44 one, plated and
unplated, which are what the taskbar and the Start list draw -- fills
in `data/msix/AppxManifest.xml.in`, indexes the tiles into
`resources.pri` with `makepri.exe`, and packs the lot with
`makeappx.exe` from the Windows 10/11 SDK, found under
`C:\Program Files (x86)\Windows Kits\10\bin\*\x64\`.

Two things about that cost an afternoon once:

- `makepri` is run over a directory holding nothing but `Assets` and
  the manifest -- pointed at the whole bundle it reads every filename in
  the GTK icon theme as a resource qualifier -- and the `<packaging>`
  block its own `createconfig` writes is stripped first, because that
  block splits the scales into separate resource packs and this is one
  package.
- The SDK tools are native Windows programs. Run them from the MSYS2
  MINGW64 shell, not from a shell whose `bash` comes from a different
  MSYS runtime (Git for Windows', say, with MSYS2 ahead of it on
  `PATH`): a child process across two runtimes gets
  `ERROR_ACCESS_DENIED` from `makepri` with nothing printed to say why.
  `C:\msys64\usr\bin\bash.exe -l` with `MSYSTEM=MINGW64` set is the
  shell that works.

Requirements the manifest is built around
([package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)):

- The version is four numbers, `Major.Minor.Build.Revision`, and the
  Store keeps the fourth for itself: it must be 0 on submission. The
  script maps meson's version to that, dropping any suffix on the way:
  `1.0.1-dev` becomes `1.0.1.0`.
- `Identity/Name` and `Identity/Publisher` have to be exactly what
  Partner Center assigns. See below.
- A full-trust desktop program declares
  `TargetDeviceFamily Name="Windows.Desktop"` with
  `MinVersion 10.0.17763.0` -- Windows 10 1809, the floor for MSIX --
  and the `runFullTrust` restricted capability.

## What the manifest declares

- **`runFullTrust`.** Office42 is an ordinary Win32 process, not a
  sandboxed app.
- **File types**, through `windows.fileTypeAssociation`: `.xlsx` and
  `.xls`, `.ods`, `.gnumeric`, `.csv`, and the old ones, `.wk1`, `.wks`,
  `.slk`, `.sylk` and `.dif`. This puts Office42 in the Open with list
  and in Settings > Default apps; it takes nothing from whatever holds
  the association now. `.html`, `.pdf` and `.tex`, which it also reads
  or writes, are left out on purpose: a spreadsheet has no business in
  the list of things that open a web page.
- **`windows.appExecutionAlias`**, so that `office42` works in a
  console. A packaged program has no fixed install path to put on
  `PATH`.
- **One `en-US` resource.** Nothing is translated through Windows
  resources; the program's own catalogues ride in `share/locale`.

## Partner Center identity

The name **Office42** is reserved, so the identity exists and
`pack-msix.sh` has it built in -- a plain run makes the package the
Store expects, with nothing to pass:

| Manifest | Value |
|---|---|
| `Package/Identity/Name` | `29567TheFreecivProject.Office42` |
| `Package/Identity/Publisher` | `CN=631F98F7-2280-49EE-8EF8-534CC36D09CF` |
| `Package/Properties/PublisherDisplayName` | `Nordstjernen` |

None of the three is ours to choose. The Package Family Name is derived
from the name and the publisher, so a package that renames either is
refused on upload. The prefix `29567TheFreecivProject` and the
publisher display name `Nordstjernen` belong to the Partner Center
account rather than to this product. The `DisplayName` has to be a
name reserved in Partner Center as well, and the reserved one is
**Office42**: a package that says "Office42 Spreadsheet" is refused on
upload with "uses a display name that you have not reserved".

Calculated from those, and worth checking a package against:

- Package Family Name `29567TheFreecivProject.Office42_ga6t65cntcpba`
- Store ID `9PHPZ3D7TGG5`
- <https://apps.microsoft.com/detail/9PHPZ3D7TGG5>, and
  `ms-windows-store://pdp/?productid=9PHPZ3D7TGG5`

That last suffix is a hash of the publisher string, which makes it a
free check that the identity is right: register the staged layout
(below) and, even when the install itself is refused, Windows names the
package it was about to make. `..._x64__ga6t65cntcpba` means the
publisher matches to the byte.

`O42_MSIX_IDENTITY_NAME`, `O42_MSIX_PUBLISHER`,
`O42_MSIX_PUBLISHER_DISPLAY` and `O42_MSIX_DISPLAY_NAME` override the
four for a different product or a different account.

## Trying it on this machine

The package is submitted unsigned, and Windows will not install an
unsigned MSIX. Two ways round that:

1. **Register the staged layout.** Needs Developer Mode on in Windows
   Settings, and no signing at all:

   ```powershell
   Add-AppxPackage -Register msix\stage\AppxManifest.xml
   ```

2. **Sign it with a test certificate**, whose subject must equal the
   manifest's `Publisher` exactly:

   ```powershell
   New-SelfSignedCertificate -Type Custom -CertStoreLocation Cert:\CurrentUser\My `
     -Subject 'CN=631F98F7-2280-49EE-8EF8-534CC36D09CF' `
     -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
   # export it to a .pfx, trust it in the machine's Trusted People store, then
   #   O42_MSIX_CERT_PFX=/c/path/test.pfx bash build-aux/pack-msix.sh builddir dist msix
   Add-AppxPackage msix\office42-<version>-win64.msix
   ```

Then check that Office42 starts from the Start menu, that an `.xlsx`
offers it under Open with, that `office42` runs in a fresh terminal,
that Tools > Python Console answers and Tools > Spelling finds a word,
and that removing it from the Start menu leaves nothing behind.

Running packaged, the program lives under
`C:\Program Files\WindowsApps\<identity>\`, which is read-only and
system-managed, and its writes to `%LOCALAPPDATA%` are redirected into
`%LOCALAPPDATA%\Packages\<identity>\LocalCache\Local\`. An embedded
database is unpacked to `%TEMP%` while the book is open and deleted
with it, which works there as anywhere.

## Before submitting

| Policy | What it wants | Office42 |
|---|---|---|
| 10.1 | An accurate listing, screenshots, a title that is the program's own | Copy below; screenshots from `docs/images/` |
| 10.2.2/3 | No malware, nothing installed on the side | One self-contained bundle; it downloads nothing |
| 10.2.7 | A clean uninstall | Managed by Windows |
| 10.5.1 | A privacy policy URL, required for every full-trust program whether or not it collects anything | Publish one at office42.net and paste the URL in |
| 10.8 | Commerce | Free, nothing to buy |
| 11.x | Content | The IARC questionnaire during submission; a spreadsheet rates for everyone |

The restricted capability needs a sentence of justification in the
submission:

```text
Capability requested: runFullTrust

Office42 Spreadsheet is a packaged Win32 desktop spreadsheet. The
capability is needed only to run the bundled desktop executable from
the MSIX package. The application does not request elevation, install
services or drivers, change system settings, manage other packages, or
collect telemetry. Its embedded Python runs only scripts the user
writes or explicitly chooses to run.
```

## The submission itself

1. Build the package -- `bash build-aux/pack-msix.sh builddir dist msix`
   -- or take `office42-windows-msix` from the Windows CI run, and
   upload `office42-<version>-win64.msix`. **Do not sign it**: the Store
   signs it after certification, and an already-signed package is
   refused.
2. Properties: category *Productivity*; Windows 10 1809 or later, x64.
3. Age rating: the IARC questionnaire.
4. Privacy policy URL. The form will not take a full-trust program
   without one.
5. The listing, en-US at least; the copy below is ready to paste.
6. Certification takes roughly one to three days.

## The listing, ready to paste

**Display name:** Office42

**Short description:** A spreadsheet in the shape of Excel 5, with 610
functions, charts, pivot tables and Python.

**Description:**

Office42 Spreadsheet is a spreadsheet the way they used to be: a grid, a
formula bar, a name box, two toolbars and sheet tabs along the bottom.
No ribbon, no account, nothing to sign in to.

It has every function Excel 2003 has and some of Gnumeric's own, 610 in
all; array formulas and dynamic arrays; charts of fifteen kinds, in two
or three dimensions, with trendlines and error bars; pivot tables, data
validation, filters, subtotals, scenarios, Goal Seek and a Solver;
conditional formatting, cell styles and rich text; notes, hyperlinks,
pictures, shapes and form controls; printing, print preview and PDF
export. Python is the macro language, with a console, a recorder and
scripts kept in the file, and a book can carry a SQLite database
inside it.

It reads and writes Excel's .xlsx and .xls, OpenDocument .ods,
Gnumeric's own format, CSV, HTML and the old Lotus, SYLK and DIF files,
and opens what LibreOffice and Excel wrote. Free software under the
GPL; the source is at github.com/office-42/office42 and the site at
office42.net.

**Features** (the Store shows up to twenty):

- 610 spreadsheet functions, every one Excel 2003 has
- Charts, trendlines, error bars, chart sheets
- Pivot tables, filters, subtotals, validation, scenarios
- Goal Seek, Solver and the statistical analysis tools
- Python console, macro recorder, scripts in the file
- Reads and writes .xlsx, .xls, .ods, .gnumeric and CSV
- Printing, print preview and PDF export
- A SQLite database inside the book
- Undo for everything, deleted sheets included
- Free software, GPL
