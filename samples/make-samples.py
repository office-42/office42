"""Write the sample books: a flat ODF source for each, then let
LibreOffice turn it into Excel's two formats.  The content is invented
here; the point of the exercise is that the .xls and .xlsx are written
by a program that is not office42."""
import io
import os
import subprocess

SOFFICE = r"C:\Program Files\LibreOffice\program\soffice.com"
OUT = r"C:\dev\office42\samples"

NS = (' xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"'
      ' xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0"'
      ' xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"'
      ' xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0"'
      ' xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"'
      ' xmlns:number="urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0"'
      ' xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2"'
      ' xmlns:calcext="urn:org:documentfoundation:names:experimental:calc:xmlns:calcext:1.0"'
      ' office:version="1.2"')


def esc(t):
    return (t.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
             .replace('"', '&quot;'))


def odf(formula):
    """ODF separates arguments with a semicolon, not a comma -- but a
    comma inside a quoted string stays as it is."""
    out, quoted = [], False
    for c in formula:
        if c == '"':
            quoted = not quoted
        out.append(';' if (c == ',' and not quoted) else c)
    return ''.join(out)


def cell(value=None, formula=None, style=None, kind='float', span=None):
    bits = []
    if style:
        bits.append('table:style-name="%s"' % style)
    if formula:
        bits.append('table:formula="of:=%s"' % esc(odf(formula)))
    if value is not None:
        if kind == 'float':
            bits.append('office:value-type="float" office:value="%s"' % value)
        elif kind == 'date':
            bits.append('office:value-type="date" office:date-value="%s"' % value)
        elif kind == 'percentage':
            bits.append('office:value-type="percentage" office:value="%s"' % value)
        elif kind == 'currency':
            bits.append('office:value-type="currency" office:currency="EUR" office:value="%s"' % value)
        elif kind == 'boolean':
            bits.append('office:value-type="boolean" office:boolean-value="%s"' % value)
        else:
            bits.append('office:value-type="string"')
    if span:
        bits.append('table:number-columns-spanned="%d" table:number-rows-spanned="1"' % span)
    text = ''
    if kind == 'string' and value is not None:
        text = '<text:p>%s</text:p>' % esc(str(value))
    inner = text
    out = '<table:table-cell %s>%s</table:table-cell>' % (' '.join(bits), inner)
    if span:
        out += '<table:covered-table-cell/>' * (span - 1)
    return out


def row(cells, style=None):
    at = ' table:style-name="%s"' % style if style else ''
    return '<table:table-row%s>%s</table:table-row>' % (at, ''.join(cells))


def document(styles, tables):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document%s office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
            '<office:automatic-styles>%s</office:automatic-styles>'
            '<office:body><office:spreadsheet>%s</office:spreadsheet></office:body>'
            '</office:document>' % (NS, styles, tables))


# ---- the styles the two books use --------------------------------------
STYLES = """
<number:number-style style:name="N-thousands"><number:number number:decimal-places="0"
  number:min-integer-digits="1" number:grouping="true"/></number:number-style>
<number:currency-style style:name="N-money"><number:text>$</number:text>
  <number:number number:decimal-places="2" number:min-integer-digits="1"
  number:grouping="true"/></number:currency-style>
<number:percentage-style style:name="N-percent"><number:number number:decimal-places="1"
  number:min-integer-digits="1"/><number:text>%</number:text></number:percentage-style>
<number:date-style style:name="N-date"><number:year number:style="long"/><number:text>-</number:text>
  <number:month number:style="long"/><number:text>-</number:text>
  <number:day number:style="long"/></number:date-style>
<number:number-style style:name="N-sci"><number:scientific-number number:decimal-places="2"
  number:min-integer-digits="1" number:min-exponent-digits="2"/></number:number-style>
<style:style style:name="co1" style:family="table-column">
  <style:table-column-properties style:column-width="1.4in"/></style:style>
<style:style style:name="ro-tall" style:family="table-row">
  <style:table-row-properties style:row-height="0.4in"/></style:style>
<style:style style:name="ce-title" style:family="table-cell">
  <style:table-cell-properties fo:background-color="#1f497d" style:vertical-align="middle"/>
  <style:text-properties fo:color="#ffffff" fo:font-size="14pt" fo:font-weight="bold"
    style:font-name="Arial"/></style:style>
<style:style style:name="ce-head" style:family="table-cell">
  <style:table-cell-properties fo:background-color="#dce6f1"
    fo:border-bottom="0.06in double #1f497d"/>
  <style:text-properties fo:font-weight="bold"/></style:style>
<style:style style:name="ce-money" style:family="table-cell" style:data-style-name="N-money">
  <style:table-cell-properties fo:border="0.5pt solid #808080"/></style:style>
<style:style style:name="ce-percent" style:family="table-cell" style:data-style-name="N-percent"/>
<style:style style:name="ce-date" style:family="table-cell" style:data-style-name="N-date"/>
<style:style style:name="ce-thousands" style:family="table-cell" style:data-style-name="N-thousands"/>
<style:style style:name="ce-sci" style:family="table-cell" style:data-style-name="N-sci"/>
<style:style style:name="ce-wrap" style:family="table-cell">
  <style:table-cell-properties fo:wrap-option="wrap" style:vertical-align="top"/></style:style>
<style:style style:name="ce-rot" style:family="table-cell">
  <style:table-cell-properties style:rotation-angle="45"/></style:style>
<style:style style:name="ce-right" style:family="table-cell">
  <style:paragraph-properties fo:text-align="end"/></style:style>
<style:style style:name="ce-centre" style:family="table-cell">
  <style:paragraph-properties fo:text-align="center"/>
  <style:text-properties fo:font-style="italic" style:text-underline-style="solid"
    style:text-underline-width="auto"/></style:style>
<style:style style:name="ce-red" style:family="table-cell">
  <style:table-cell-properties fo:background-color="#ffff99"/>
  <style:text-properties fo:color="#c00000" fo:font-weight="bold"/></style:style>
"""


# ---- book one: formats --------------------------------------------------
def formats_book():
    rows = []
    rows.append(row([cell('A spreadsheet of formats', kind='string', style='ce-title', span=4)],
                    style='ro-tall'))
    rows.append(row([cell('What', kind='string', style='ce-head'),
                     cell('Shown', kind='string', style='ce-head'),
                     cell('Number', kind='string', style='ce-head'),
                     cell('Note', kind='string', style='ce-head')]))
    lines = [
        ('Thousands', 1234567.891, 'ce-thousands', 'grouped, no decimals'),
        ('Money', 1234.5, 'ce-money', 'currency, two decimals, a border'),
        ('Percent', 0.0725, 'ce-percent', 'one decimal and a sign'),
        ('Scientific', 0.000123456, 'ce-sci', 'two decimals, two exponent digits'),
        ('Negative', -4321.99, 'ce-money', 'the same money format, below zero'),
        ('Zero', 0, 'ce-thousands', 'nothing much'),
    ]
    for what, number, style, note in lines:
        rows.append(row([cell(what, kind='string'),
                         cell(number, style=style),
                         cell(number),
                         cell(note, kind='string')]))
    rows.append(row([cell('Date', kind='string'),
                     cell('2026-08-29', kind='date', style='ce-date'),
                     cell(46264),
                     cell('a date style, and its serial number', kind='string')]))
    rows.append(row([cell('Boolean', kind='string'),
                     cell('true', kind='boolean'),
                     cell(1),
                     cell('TRUE as a value, not as a word', kind='string')]))
    rows.append(row([]))
    rows.append(row([cell('Alignment', kind='string', style='ce-head'),
                     cell('right', kind='string', style='ce-right'),
                     cell('centred, italic, underlined', kind='string', style='ce-centre'),
                     cell('turned 45 degrees', kind='string', style='ce-rot')],
                    style='ro-tall'))
    rows.append(row([cell('Wrapping', kind='string'),
                     cell('a long line of text that has to be wrapped inside its cell '
                          'rather than spilling over the next one', kind='string',
                          style='ce-wrap', span=3)], style='ro-tall'))
    rows.append(row([cell('Colour', kind='string'),
                     cell('bold red on yellow', kind='string', style='ce-red')]))

    table = ('<table:table table:name="Formats">'
             '<table:table-column table:style-name="co1" table:number-columns-repeated="4"/>'
             + ''.join(rows) + '</table:table>')
    return document(STYLES, table)


# ---- book two: formulas -------------------------------------------------
FORMULAS = [
    ('SUM', 'SUM(D2:D11)'),
    ('AVERAGE', 'AVERAGE(D2:D11)'),
    ('MEDIAN', 'MEDIAN(D2:D11)'),
    ('STDEV', 'STDEV(D2:D11)'),
    ('MIN and MAX', 'MIN(D2:D11)*MAX(D2:D11)'),
    ('COUNT and COUNTA', 'COUNT(D2:D11)+COUNTA(B2:B11)'),
    ('COUNTIF', 'COUNTIF(C2:C11,"North")'),
    ('SUMIF', 'SUMIF(C2:C11,"North",D2:D11)'),
    ('SUMIFS', 'SUMIFS(D2:D11,C2:C11,"South",D2:D11,">100")'),
    ('AVERAGEIF', 'AVERAGEIF(C2:C11,"North",D2:D11)'),
    ('SUMPRODUCT', 'SUMPRODUCT(D2:D11,E2:E11)'),
    ('VLOOKUP exact', 'VLOOKUP("Gadget",B2:D11,3,FALSE())'),
    ('INDEX and MATCH', 'INDEX(D2:D11,MATCH("Widget",B2:B11,0))'),
    ('IF and AND', 'IF(AND(D2>100,C2="North"),"yes","no")'),
    ('IFS-like nesting', 'IF(D2>500,"big",IF(D2>100,"middling","small"))'),
    ('ROUND family', 'ROUND(D2,1)+ROUNDUP(D3,0)+ROUNDDOWN(D4,0)'),
    ('MOD and INT', 'MOD(17,5)*INT(7.9)'),
    ('POWER and SQRT', 'POWER(2,10)+SQRT(144)'),
    ('LOG and EXP', 'LN(EXP(3))+LOG10(1000)'),
    ('Trigonometry', 'SIN(PI()/6)+COS(0)+TAN(PI()/4)'),
    ('TEXT', 'TEXT(1234.567,"#,##0.00")'),
    ('CONCATENATE', 'CONCATENATE(B2," in ",C2)'),
    ('LEFT MID RIGHT', 'LEFT(B2,3)&MID(B2,2,2)&RIGHT(B2,2)'),
    ('LEN TRIM UPPER', 'LEN(TRIM("  a b  "))&UPPER("x")'),
    ('SUBSTITUTE', 'SUBSTITUTE("a-b-c","-","+")'),
    ('FIND and SEARCH', 'FIND("d",B2)+SEARCH("D",B2)'),
    ('DATE parts', 'YEAR(F2)*10000+MONTH(F2)*100+DAY(F2)'),
    ('EOMONTH', 'DAY(EOMONTH(F2,1))'),
    ('WEEKDAY', 'WEEKDAY(F2,2)'),
    ('NETWORKDAYS', 'NETWORKDAYS(F2,F11)'),
    ('DATEDIF-like', 'DAYS360(F2,F11)'),
    ('PMT', 'PMT(0.05/12,360,200000)'),
    ('FV and PV', 'FV(0.04,10,-100,0,0)+PV(0.04,10,-100,0,0)'),
    ('NPV and IRR', 'NPV(0.1,D2:D5)'),
    ('RATE', 'RATE(120,-1000,100000)'),
    ('SLN and DDB', 'SLN(10000,1000,5)+DDB(10000,1000,5,1)'),
    ('CHOOSE', 'CHOOSE(2,"a","b","c")'),
    ('OFFSET', 'OFFSET(D2,2,0)'),
    ('INDIRECT', 'INDIRECT("D2")'),
    ('RANK and LARGE', 'RANK(D2,D2:D11)+LARGE(D2:D11,2)'),
    ('PERCENTILE', 'PERCENTILE(D2:D11,0.75)'),
    ('CORREL and SLOPE', 'CORREL(D2:D11,E2:E11)+SLOPE(D2:D11,E2:E11)'),
    ('FORECAST', 'FORECAST(11,D2:D11,E2:E11)'),
    ('NORMDIST', 'NORMDIST(1.5,0,1,TRUE())'),
    ('BINOMDIST', 'BINOMDIST(3,10,0.5,FALSE())'),
    ('CHIDIST', 'CHIDIST(3.84,1)'),
    ('An array formula', 'SUM(D2:D11*E2:E11)'),
    ('ISNUMBER and ISTEXT', 'IF(ISNUMBER(D2),1,0)+IF(ISTEXT(B2),1,0)'),
    ('ISERROR', 'IF(ISERROR(1/0),"caught","no")'),
    ('Nested references', 'SUM(D2:INDEX(D2:D11,5))'),
]

PRODUCTS = ['Widget', 'Gadget', 'Doohickey', 'Sprocket', 'Flange',
            'Grommet', 'Bracket', 'Washer', 'Bearing', 'Coupling']
REGIONS = ['North', 'South', 'North', 'East', 'South',
           'North', 'West', 'South', 'North', 'East']
AMOUNTS = [125.5, 890.25, 45, 612.75, 233.1, 1050, 78.4, 340.6, 199.99, 505]
UNITS = [12, 45, 3, 30, 18, 60, 6, 24, 15, 33]
DATES = ['2026-01-05', '2026-01-19', '2026-02-02', '2026-02-16', '2026-03-02',
         '2026-03-16', '2026-04-06', '2026-04-20', '2026-05-04', '2026-05-18']


def formulas_book():
    """One sheet: the table at the top, the checks under it, so that
    every reference stays on the sheet it is written on."""
    rows = [row([cell('Row', kind='string', style='ce-head'),
                 cell('Product', kind='string', style='ce-head'),
                 cell('Region', kind='string', style='ce-head'),
                 cell('Amount', kind='string', style='ce-head'),
                 cell('Units', kind='string', style='ce-head'),
                 cell('Date', kind='string', style='ce-head')])]
    for i in range(10):
        rows.append(row([cell(i + 1),
                         cell(PRODUCTS[i], kind='string'),
                         cell(REGIONS[i], kind='string'),
                         cell(AMOUNTS[i], style='ce-money'),
                         cell(UNITS[i]),
                         cell(DATES[i], kind='date', style='ce-date')]))
    rows.append(row([]))
    rows.append(row([cell('What', kind='string', style='ce-head'),
                     cell('Formula', kind='string', style='ce-head'),
                     cell('Value', kind='string', style='ce-head')]))
    for what, f in FORMULAS:
        rows.append(row([cell(what, kind='string'),
                         cell("'" + f, kind='string'),
                         cell(formula=f)]))
    table = ('<table:table table:name="Formulas">'
             '<table:table-column table:style-name="co1" table:number-columns-repeated="6"/>'
             + ''.join(rows) + '</table:table>')
    return document(STYLES, table)


def convert(path, target, suffix):
    subprocess.run([SOFFICE, '--headless', '--convert-to', target, '--outdir', OUT, path],
                   capture_output=True)
    made = os.path.splitext(path)[0] + suffix
    return os.path.exists(made)


os.makedirs(OUT, exist_ok=True)
for name, text in (('formats', formats_book()), ('formulas', formulas_book())):
    path = os.path.join(OUT, name + '.fods')
    io.open(path, 'w', encoding='utf-8', newline='\n').write(text)
    ok_xlsx = convert(path, 'xlsx:Calc MS Excel 2007 XML', '.xlsx')
    ok_xls = convert(path, 'xls:MS Excel 97', '.xls')
    print('%-10s fods ok  xlsx %s  xls %s' % (name, ok_xlsx, ok_xls))
