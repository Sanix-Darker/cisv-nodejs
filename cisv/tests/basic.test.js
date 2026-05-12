const { cisvParser, TransformType } = require('../build/Release/cisv');
const assert = require('assert');
const fs = require('fs');
const path = require('path');

function fetchAllRows(parser) {
  const rows = [];
  let row;
  while ((row = parser.fetchRow()) !== null) {
    rows.push(row);
  }
  return rows;
}

describe('CSV Parser Core Functionality', () => {
  const testDir = path.join(__dirname, 'fixtures');
  const testFile = path.join(testDir, 'test.csv');
  const largeFile = path.join(testDir, 'large.csv');
  const tsvFile = path.join(testDir, 'test.tsv');
  const quotedFile = path.join(testDir, 'quoted.csv');
  const countControlFile = path.join(testDir, 'count-controls.csv');
  const escapedCountFile = path.join(testDir, 'count-escaped.csv');
  const iteratorConfigFile = path.join(testDir, 'iterator-config.csv');
  const iteratorEmptyFile = path.join(testDir, 'iterator-empty.csv');
  const iteratorLimitFile = path.join(testDir, 'iterator-limit.csv');
  const iteratorBadAfterQuoteFile = path.join(testDir, 'iterator-bad-after-quote.csv');
  const iteratorBadUnterminatedFile = path.join(testDir, 'iterator-bad-unterminated.csv');

  before(() => {
    if (!fs.existsSync(testDir)) fs.mkdirSync(testDir, { recursive: true });

    // Basic test file - NOTE: The parser includes ALL rows including header
    fs.writeFileSync(testFile,
      'id,name,email\n1,John,john@test.com\n2,Jane Doe,jane@test.com\n3,"Alex ""The Boss""",alex@test.com');

    // TSV file for configuration testing
    fs.writeFileSync(tsvFile,
      'id\tname\temail\n1\tJohn\tjohn@test.com\n2\tJane\tjane@test.com');

    // File with complex quoting
    fs.writeFileSync(quotedFile,
      'name,description,price\n' +
      '"Product A","A simple, basic product",10.99\n' +
      '"Product B","Contains ""quotes"" and, commas",20.50\n' +
      '"Product C","Multi\nline\ndescription",15.00');

    fs.writeFileSync(countControlFile,
      '  #skip\n' +
      '\n' +
      'h1,h2\n' +
      ',,\n' +
      '"#keep",x\n');

    fs.writeFileSync(escapedCountFile,
      'id,payload\n' +
      '1,"line1\\\nline2 with \\"quote\\""\n');

    fs.writeFileSync(iteratorConfigFile,
      '  #skip\n' +
      'id,msg\n' +
      '1,"hello \\"quoted\\""\n' +
      '2,tail\n');

    fs.writeFileSync(iteratorEmptyFile,
      '\n' +
      '""\n' +
      '#skip\n' +
      '"#keep"\n' +
      ',,\n');

    fs.writeFileSync(iteratorLimitFile, 'a,b\n123456789,2\n');
    fs.writeFileSync(iteratorBadAfterQuoteFile, '"a"x,b\n');
    fs.writeFileSync(iteratorBadUnterminatedFile, '"unterminated\n');

    // Generate large test file (1000 rows)
    let largeContent = 'id,value\n';
    for (let i = 0; i < 1000; i++) {
      largeContent += `${i},Value ${i}\n`;
    }
    fs.writeFileSync(largeFile, largeContent);
  });

  after(() => {
    [
      testFile,
      largeFile,
      tsvFile,
      quotedFile,
      countControlFile,
      escapedCountFile,
      iteratorConfigFile,
      iteratorEmptyFile,
      iteratorLimitFile,
      iteratorBadAfterQuoteFile,
      iteratorBadUnterminatedFile
    ].forEach(file => {
      if (fs.existsSync(file)) fs.unlinkSync(file);
    });
    if (fs.existsSync(testDir)) fs.rmdirSync(testDir);
  });

  describe('Synchronous Parsing', () => {
    it('should parse basic CSV correctly including header', () => {
      const parser = new cisvParser();
      const rows = parser.parseSync(testFile);

      // Parser returns ALL rows including header
      assert.strictEqual(rows.length, 4, 'Should have 4 rows total (header + 3 data rows)');
      assert.deepStrictEqual(rows[0], ['id', 'name', 'email'], 'First row should be header');
      assert.deepStrictEqual(rows[1], ['1', 'John', 'john@test.com']);
      assert.deepStrictEqual(rows[2], ['2', 'Jane Doe', 'jane@test.com']);
    });

    it('should handle quoted fields with quotes correctly', () => {
      const parser = new cisvParser();
      const rows = parser.parseSync(testFile);

      // RFC 4180: doubled quotes ("") inside quoted fields become single quote
      assert.strictEqual(rows[3][1], 'Alex "The Boss"', 'Should handle escaped quotes');
      assert.strictEqual(rows[3][2], 'alex@test.com');
    });

    it('should handle large files efficiently', () => {
      const parser = new cisvParser();
      const start = Date.now();
      const rows = parser.parseSync(largeFile);
      const duration = Date.now() - start;

      assert.strictEqual(rows.length, 1001, 'Should have header + 1000 rows');
      assert.strictEqual(rows[500][1], 'Value 499', 'Should correctly parse middle row');
      assert.ok(duration < 100, `Should parse 1000 rows in under 100ms (took ${duration}ms)`);
    });

    it('should get statistics', () => {
      const parser = new cisvParser();
      parser.parseSync(testFile);
      const stats = parser.getStats();

      assert.strictEqual(stats.rowCount, 4);
      assert.strictEqual(stats.fieldCount, 3);
      assert.ok(stats.totalBytes === 0 || stats.totalBytes > 0); // May not be set for file parsing
      assert.ok(stats.parseTime >= 0);
    });
  });

  describe('Configuration Support', () => {
    it('should parse TSV files with tab delimiter', () => {
      const parser = new cisvParser({ delimiter: '\t' });
      const rows = parser.parseSync(tsvFile);

      assert.strictEqual(rows.length, 3);
      assert.deepStrictEqual(rows[0], ['id', 'name', 'email']);
      assert.deepStrictEqual(rows[1], ['1', 'John', 'john@test.com']);
    });

    it('should trim whitespace when configured', () => {
      const parser = new cisvParser({ trim: true });
      const csvWithSpaces = 'name,age\n  John  ,  30  \n Jane , 25 ';
      const rows = parser.parseString(csvWithSpaces);

      assert.deepStrictEqual(rows[1], ['John', '30']);
      assert.deepStrictEqual(rows[2], ['Jane', '25']);
    });

    it('should skip physically empty lines without dropping empty-field rows', () => {
      const parser = new cisvParser({ skipEmptyLines: true });
      const csvWithEmpty = 'a,b,c\n\n,,\n1,2,3\n';
      const rows = parser.parseString(csvWithEmpty);

      assert.strictEqual(rows.length, 3, 'Should skip only the blank physical line');
      assert.deepStrictEqual(rows[1], ['', '', '']);
      assert.deepStrictEqual(rows[2], ['1', '2', '3']);
    });

    it('should handle line range selection', () => {
      const parser = new cisvParser({ fromLine: 2, toLine: 3 });
      const rows = parser.parseSync(testFile);

      // fromLine and toLine are 1-based and apply to all lines including header
      assert.strictEqual(rows.length, 2, 'Should only parse lines 2-3');
      assert.deepStrictEqual(rows[0], ['1', 'John', 'john@test.com']);
      assert.deepStrictEqual(rows[1], ['2', 'Jane Doe', 'jane@test.com']);

      const stringRows = parser.parseString('h1,h2\ns1,s2\ns3,s4\ns5,s6');
      assert.deepStrictEqual(stringRows, [['s1', 's2'], ['s3', 's4']]);
    });

    it('should handle comment lines', () => {
      const parser = new cisvParser({ comment: '#', trim: true });
      const csvWithComments = '  # This is a comment\nname,age\n# Another comment\nJohn,30\nJane,25';
      const rows = parser.parseString(csvWithComments);

      assert.strictEqual(rows.length, 3, 'Should skip comment lines');
      assert.deepStrictEqual(rows[0], ['name', 'age']);
      assert.deepStrictEqual(rows[2], ['Jane', '25']);
    });

    it('should handle custom escape in parse paths', () => {
      const parser = new cisvParser({ escape: '\\' });
      const rows = parser.parseString('id,msg\n1,"hello \\"quoted\\" value"');

      assert.deepStrictEqual(rows[1], ['1', 'hello "quoted" value']);
    });

    it('should preserve size_t-sized maxRowSize values', () => {
      const largeLimit = 2 ** 40;
      const parser = new cisvParser({ maxRowSize: largeLimit });

      assert.strictEqual(parser.getConfig().maxRowSize, largeLimit);
    });

    it('should validate parser config types, ranges, and conflicts', () => {
      assert.throws(() => new cisvParser({ maxRowSize: -1 }), /maxRowSize is out of range/);
      assert.throws(() => new cisvParser({ maxRowSize: 1.5 }), /maxRowSize is out of range/);
      assert.throws(() => new cisvParser({ fromLine: 3, toLine: 2 }), /toLine must be >= fromLine/);
      assert.throws(() => new cisvParser({ delimiter: '"' }), /delimiter and quote cannot be the same/);
      assert.throws(() => new cisvParser({ escape: ',' }), /escape and delimiter cannot be the same/);
      assert.throws(() => new cisvParser({ comment: ',' }), /comment cannot conflict/);
      assert.throws(() => new cisvParser({ trim: 'yes' }), /trim must be a boolean/);
    });

    it('should dynamically change configuration', () => {
      const parser = new cisvParser();

      // Parse as CSV first
      let rows = parser.parseString('a,b,c\n1,2,3');
      assert.strictEqual(rows[0].length, 3);

      // Change to TSV
      parser.setConfig({ delimiter: '\t' });
      rows = parser.parseString('a\tb\tc\n1\t2\t3');
      assert.strictEqual(rows[0].length, 3);
      assert.deepStrictEqual(rows[0], ['a', 'b', 'c']);

      // Verify config was changed
      const config = parser.getConfig();
      assert.strictEqual(config.delimiter, '\t');
    });
  });

  describe('String Parsing', () => {
    it('should parse CSV string correctly', () => {
      const parser = new cisvParser();
      const csvString = 'name,age\nJohn,30\nJane,25';
      const rows = parser.parseString(csvString);

      assert.strictEqual(rows.length, 3);
      assert.deepStrictEqual(rows[0], ['name', 'age']);
      assert.deepStrictEqual(rows[1], ['John', '30']);
    });

    it('should handle complex quoted fields', () => {
      const parser = new cisvParser();
      const rows = parser.parseSync(quotedFile);

      // Column indices: 0=name, 1=description, 2=price
      assert.strictEqual(rows[1][1], 'A simple, basic product');
      assert.strictEqual(rows[2][1], 'Contains "quotes" and, commas');
      // Note: Multi-line within quotes may need special handling
    });

    it('should parse Buffer input without truncating embedded NUL bytes', () => {
      const parser = new cisvParser();
      const rows = parser.parseString(Buffer.from('a,b\nx\0y,z\n', 'latin1'));

      assert.strictEqual(rows.length, 2);
      assert.strictEqual(rows[1][0].length, 3);
      assert.strictEqual(rows[1][0].charCodeAt(1), 0);
      assert.deepStrictEqual(rows[1], ['x\0y', 'z']);
    });

    it('should replace invalid UTF-8 bytes from Buffer input without throwing', () => {
      const parser = new cisvParser();
      const rows = parser.parseString(Buffer.from([0x61, 0x2c, 0x62, 0x0a, 0x31, 0x2c, 0xff, 0x0a]));

      assert.deepStrictEqual(rows[0], ['a', 'b']);
      assert.strictEqual(rows[1][1], '\uFFFD');
    });
  });

  describe('Streaming API', () => {
    // FIXME:
    // it('should process data in chunks', (done) => {
    //   const parser = new cisvParser();
    //   const stream = fs.createReadStream(testFile, { highWaterMark: 16 }); // Small chunks

    //   let chunks = 0;
    //   stream.on('data', chunk => {
    //     chunks++;
    //     parser.write(chunk);
    //   });

    //   stream.on('end', () => {
    //     parser.end();
    //     const rows = parser.getRows();

    //     assert.strictEqual(rows.length, 4, 'Should have all rows after streaming');
    //     assert.ok(chunks > 1, 'Should have processed multiple chunks');
    //     parser.clear(); // Clean up
    //     done();
    //   });
    // });

    it('should handle partial chunks correctly', () => {
      const parser = new cisvParser();

      // Write partial lines
      parser.write(Buffer.from('id,name,emai'));
      parser.write(Buffer.from('l\n1,John,john@test.com'));
      parser.end();

      const rows = parser.getRows();
      assert.strictEqual(rows.length, 2);
      assert.deepStrictEqual(rows[0], ['id', 'name', 'email']);
      assert.deepStrictEqual(rows[1], ['1', 'John', 'john@test.com']);

      parser.clear();
    });

    it('should clear accumulated data', () => {
      const parser = new cisvParser();

      parser.parseString('a,b\n1,2');
      assert.strictEqual(parser.getRows().length, 2);

      parser.clear();
      assert.strictEqual(parser.getRows().length, 0);

      parser.parseString('c,d\n3,4');
      assert.strictEqual(parser.getRows().length, 2);
    });
  });

  describe('Transform Support', () => {
    it('should apply uppercase transform', () => {
      const parser = new cisvParser();
      parser.transform(1, 'uppercase'); // Transform name column

      const rows = parser.parseSync(testFile);
      assert.strictEqual(rows[1][1], 'JOHN');
      assert.strictEqual(rows[2][1], 'JANE DOE');
    });

    it('should apply multiple transforms', () => {
      const parser = new cisvParser();
      parser
        .transform(0, 'trim')
        .transform(1, 'uppercase')
        .transform(2, 'lowercase');

      const csvString = '  1  ,john,JOHN@TEST.COM\n  2  ,jane,JANE@TEST.COM';
      const rows = parser.parseString(csvString);

      assert.strictEqual(rows[0][0], '1');
      assert.strictEqual(rows[0][1], 'JOHN');
      assert.strictEqual(rows[0][2], 'john@test.com');
    });

    it('should apply JavaScript function transforms', () => {
      const parser = new cisvParser();
      parser.transform(1, (value) => value.split(' ')[0]); // Get first name only

      const rows = parser.parseSync(testFile);
      assert.strictEqual(rows[2][1], 'Jane'); // "Jane Doe" -> "Jane"
    });

    it('should apply transform to all fields', () => {
      const parser = new cisvParser();
      parser.transform(-1, 'trim'); // Apply to all fields

      const csvString = '  a  ,  b  ,  c  \n  1  ,  2  ,  3  ';
      const rows = parser.parseString(csvString);

      assert.deepStrictEqual(rows[0], ['a', 'b', 'c']);
      assert.deepStrictEqual(rows[1], ['1', '2', '3']);
    });

    it('should clear native transforms', () => {
      const parser = new cisvParser();
      parser.transform(0, 'uppercase');

      let rows = parser.parseString('name\njohn');
      assert.strictEqual(rows[1][0], 'JOHN');

      parser.clearTransforms();
      rows = parser.parseString('name\njane');
      assert.strictEqual(rows[1][0], 'jane');
      assert.strictEqual(parser.getTransformInfo().cTransformCount, 0);
    });

    it('should remove native transforms by field index', () => {
      const parser = new cisvParser();
      parser.transform(0, 'uppercase').transform(1, 'lowercase');

      parser.removeTransform(0);
      const rows = parser.parseString('Name,Email\nJohn,JOHN@TEST.COM');

      assert.strictEqual(rows[1][0], 'John');
      assert.strictEqual(rows[1][1], 'john@test.com');

      const info = parser.getTransformInfo();
      assert.strictEqual(info.cTransformCount, 1);
      assert.deepStrictEqual(info.fieldIndices, [1]);
    });

    it('should remove JavaScript transforms by field index and reset references', () => {
      const parser = new cisvParser();
      parser.transform(0, value => value.toUpperCase());
      parser.removeTransform(0);

      const rows = parser.parseString('name\njohn');
      assert.strictEqual(rows[1][0], 'john');
      assert.strictEqual(parser.getTransformInfo().jsTransformCount, 0);
    });

    it('should remove native and JavaScript transforms by field name', () => {
      const parser = new cisvParser();
      parser.setHeaderFields(['id', 'name']);
      parser
        .transformByName('id', value => `#${value}`)
        .transformByName('name', 'uppercase');

      parser.removeTransformByName('id');
      parser.removeTransformByName('name');

      const rows = parser.parseString('id,name\n1,john');
      assert.deepStrictEqual(rows[1], ['1', 'john']);

      const info = parser.getTransformInfo();
      assert.strictEqual(info.cTransformCount, 0);
      assert.strictEqual(info.jsTransformCount, 0);
      assert.deepStrictEqual(info.fieldIndices, []);
    });

    it('should get transform info', () => {
      const parser = new cisvParser();
      parser.transform(0, 'uppercase');
      parser.transform(1, 'lowercase');
      parser.transform(2, (v) => v);

      const info = parser.getTransformInfo();
      assert.strictEqual(info.cTransformCount, 2);
      assert.strictEqual(info.jsTransformCount, 1);
      assert.deepStrictEqual(info.fieldIndices.sort((a, b) => a - b), [0, 1, 2]);
    });

    it('should reject unsupported crypto transform names', () => {
      const parser = new cisvParser();

      assert.throws(() => parser.transform(0, 'hash_sha256'), /Unknown transform type/);
      assert.throws(() => parser.transform(0, 'sha256'), /Unknown transform type/);
      assert.strictEqual(Object.prototype.hasOwnProperty.call(TransformType, 'HASH_SHA256'), false);
    });
  });

  describe('Row Counting', () => {
    it('should count rows without parsing', () => {
      const count = cisvParser.countRows(largeFile);
      assert.strictEqual(count, 1001);
    });

    it('should count rows with configuration', () => {
      const count = cisvParser.countRowsWithConfig(tsvFile, { delimiter: '\t' });
      assert.strictEqual(count, 3);
    });

    it('should count rows with semantic controls', () => {
      const count = cisvParser.countRowsWithConfig(countControlFile, {
        comment: '#',
        skipEmptyLines: true,
        trim: true
      });
      assert.strictEqual(count, 3);
    });

    it('should count rows with line ranges', () => {
      const count = cisvParser.countRowsWithConfig(largeFile, {
        fromLine: 2,
        toLine: 11
      });
      assert.strictEqual(count, 10);
    });

    it('should count rows with custom escape', () => {
      const count = cisvParser.countRowsWithConfig(escapedCountFile, {
        escape: '\\'
      });
      assert.strictEqual(count, 2);
    });

    it('should count quoted multiline rows', () => {
      const count = cisvParser.countRowsWithConfig(quotedFile, {});
      assert.strictEqual(count, 4);
    });

    it('should validate count config options', () => {
      assert.throws(
        () => cisvParser.countRowsWithConfig(testFile, { escape: 'xx' }),
        /escape must be exactly 1 character/
      );
      assert.throws(
        () => cisvParser.countRowsWithConfig(testFile, { fromLine: -1 }),
        /fromLine is out of range/
      );
      assert.throws(
        () => cisvParser.countRowsWithConfig(testFile, { skipEmptyLines: 'yes' }),
        /skipEmptyLines must be a boolean/
      );
      assert.throws(
        () => cisvParser.countRowsWithConfig(testFile, { maxRowSize: 1.5 }),
        /maxRowSize is out of range/
      );
      assert.throws(
        () => cisvParser.countRowsWithConfig(testFile, { delimiter: '"' }),
        /delimiter and quote cannot be the same/
      );
      assert.throws(
        () => cisvParser.countRowsWithConfig(testFile, { fromLine: 3, toLine: 2 }),
        /toLine must be >= fromLine/
      );
    });
  });

  describe('Iterator API', () => {
    it('should use parser config for escape, comments, trimming, and line controls', () => {
      const parser = new cisvParser({
        escape: '\\',
        comment: '#',
        trim: true,
        fromLine: 1,
        toLine: 3
      });

      parser.openIterator(iteratorConfigFile);
      const rows = fetchAllRows(parser);
      parser.closeIterator();

      assert.deepStrictEqual(rows, [
        ['id', 'msg'],
        ['1', 'hello "quoted"']
      ]);
    });

    it('should preserve quoted empty and quoted comment rows with skipEmptyLines', () => {
      const parser = new cisvParser({
        comment: '#',
        skipEmptyLines: true
      });

      parser.openIterator(iteratorEmptyFile);
      const rows = fetchAllRows(parser);
      parser.closeIterator();

      assert.deepStrictEqual(rows, [
        [''],
        ['#keep'],
        ['', '', '']
      ]);
    });

    it('should enforce maxRowSize during iteration', () => {
      const parser = new cisvParser({ maxRowSize: 8 });
      parser.openIterator(iteratorLimitFile);

      try {
        assert.throws(() => fetchAllRows(parser), /Error reading CSV row/);
      } finally {
        parser.closeIterator();
      }
    });

    it('should reject malformed quoted fields during iteration', () => {
      const parser = new cisvParser();

      parser.openIterator(iteratorBadAfterQuoteFile);
      try {
        assert.throws(() => fetchAllRows(parser), /Error reading CSV row/);
      } finally {
        parser.closeIterator();
      }

      parser.openIterator(iteratorBadUnterminatedFile);
      try {
        assert.throws(() => fetchAllRows(parser), /Error reading CSV row/);
      } finally {
        parser.closeIterator();
      }
    });

    it('should repeatedly open, fetch, and close without changing results', () => {
      const parser = new cisvParser({ skipEmptyLines: true });

      for (let i = 0; i < 50; i++) {
        parser.openIterator(iteratorEmptyFile);
        const rows = fetchAllRows(parser);
        parser.closeIterator();
        assert.strictEqual(rows.length, 4);
      }
    });
  });

  // FIXME: (error core dumpe)
  // describe('Error Handling', () => {
  //   it('should throw on non-existent file', () => {
  //     const parser = new cisvParser();
  //     assert.throws(
  //       () => parser.parseSync('./nonexistent.csv'),
  //       /parse error/,
  //       'Should throw parse error for non-existent file'
  //     );
  //   });

  //   it('should throw on invalid write arguments', () => {
  //     const parser = new cisvParser();

  //     assert.throws(() => parser.write(123), /Expected Buffer or String/);
  //     assert.throws(() => parser.write({}), /Expected Buffer or String/);
  //     assert.throws(() => parser.write(null), /Expected Buffer or String/);
  //     assert.throws(() => parser.write(), /Expected one argument/);
  //   });

  //   it('should accept both Buffer and String in write', () => {
  //     const parser = new cisvParser();

  //     assert.doesNotThrow(() => parser.write(Buffer.from('test')));
  //     assert.doesNotThrow(() => parser.write('test'));
  //   });

  //   it('should handle parser destruction', () => {
  //     const parser = new cisvParser();
  //     parser.parseString('a,b\n1,2');

  //     parser.destroy();

  //     assert.throws(
  //       () => parser.parseString('c,d\n3,4'),
  //       /Parser has been destroyed/,
  //       'Should not allow operations after destroy'
  //     );
  //   });

  //   it('should handle max row size limit', () => {
  //     const parser = new cisvParser({ maxRowSize: 10 });

  //     // This should trigger row size limit
  //     const longRow = 'a,b,c\n' + 'x'.repeat(20) + ',y,z\nshort,row,here';
  //     const rows = parser.parseString(longRow);

  //     // Implementation should skip the oversized row
  //     // Exact behavior depends on skipLinesWithError setting
  //   });
  // });

  describe('Async Parsing', () => {
    it('should parse asynchronously', async () => {
      const parser = new cisvParser();
      const rows = await parser.parse(testFile);

      assert.strictEqual(rows.length, 4);
      assert.deepStrictEqual(rows[0], ['id', 'name', 'email']);
    });

    it('should parse synchronously in parallel mode', () => {
      const parser = new cisvParser();
      const rows = parser.parseSyncParallel(testFile, 2);

      assert.strictEqual(rows.length, 4);
      assert.deepStrictEqual(rows[0], ['id', 'name', 'email']);
    });

    it('should parse asynchronously in parallel mode', async () => {
      const parser = new cisvParser();
      const rows = await parser.parseParallel(testFile, 2);

      assert.strictEqual(rows.length, 4);
      assert.deepStrictEqual(rows[0], ['id', 'name', 'email']);
    });

    it('should reject promise on error', async () => {
      const parser = new cisvParser();

      try {
        await parser.parse('./nonexistent.csv');
        assert.fail('Should have thrown error');
      } catch (error) {
        assert.ok(error.message.includes('parse error'));
      }
    });
  });
});

// Additional test suite for advanced features
describe('Advanced CSV Features', () => {
  const testDir = path.join(__dirname, 'fixtures');

  before(() => {
    if (!fs.existsSync(testDir)) fs.mkdirSync(testDir, { recursive: true });
  });

  after(() => {
    if (fs.existsSync(testDir)) {
      fs.readdirSync(testDir).forEach(file => {
        fs.unlinkSync(path.join(testDir, file));
      });
      fs.rmdirSync(testDir);
    }
  });

  describe('Edge Cases', () => {
    it('should handle empty file', () => {
      const emptyFile = path.join(testDir, 'empty.csv');
      fs.writeFileSync(emptyFile, '');

      const parser = new cisvParser();
      const rows = parser.parseSync(emptyFile);

      assert.strictEqual(rows.length, 0);
    });

    it('should handle file with only header', () => {
      const headerOnly = path.join(testDir, 'header-only.csv');
      fs.writeFileSync(headerOnly, 'col1,col2,col3');

      const parser = new cisvParser();
      const rows = parser.parseSync(headerOnly);

      assert.strictEqual(rows.length, 1);
      assert.deepStrictEqual(rows[0], ['col1', 'col2', 'col3']);
    });

    it('should handle CRLF line endings', () => {
      const parser = new cisvParser();
      const crlf = 'a,b\r\n1,2\r\n3,4';
      const rows = parser.parseString(crlf);

      assert.strictEqual(rows.length, 3);
      assert.deepStrictEqual(rows[1], ['1', '2']);
    });

    it('should handle BOM (Byte Order Mark)', () => {
      const bomFile = path.join(testDir, 'bom.csv');
      // UTF-8 BOM + CSV content
      fs.writeFileSync(bomFile, '\ufeffa,b\n1,2');

      const parser = new cisvParser();
      const rows = parser.parseSync(bomFile);

      // Parser should handle or skip BOM
      assert.ok(rows.length > 0);
    });
  });

  describe('Performance', () => {
    it('should handle very wide rows efficiently', () => {
      const wideFile = path.join(testDir, 'wide.csv');
      const cols = Array.from({ length: 100 }, (_, i) => `col${i}`);
      const values = Array.from({ length: 100 }, (_, i) => `val${i}`);

      fs.writeFileSync(wideFile, cols.join(',') + '\n' + values.join(','));

      const parser = new cisvParser();
      const rows = parser.parseSync(wideFile);

      assert.strictEqual(rows[0].length, 100);
      assert.strictEqual(rows[1][99], 'val99');
    });

    it('should handle very long fields efficiently', () => {
      const longFieldFile = path.join(testDir, 'long-field.csv');
      const longValue = 'x'.repeat(10000);

      fs.writeFileSync(longFieldFile, `short,long\nval,"${longValue}"`);

      const parser = new cisvParser();
      const rows = parser.parseSync(longFieldFile);

      assert.strictEqual(rows[1][1].length, 10000);
    });
  });
});
