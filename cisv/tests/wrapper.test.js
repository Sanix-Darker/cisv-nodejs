const { cisvParser } = require('../cisv');
const assert = require('assert');

function parseWritten(input, options) {
  const parser = new cisvParser(options);
  parser.write(input);
  parser.end();
  return parser.getRows();
}

describe('Package wrapper fast path', () => {
  it('preserves empty LF rows in simple Buffer input', () => {
    assert.deepStrictEqual(parseWritten(Buffer.from('\n\n')), [[''], ['']]);
  });

  it('falls back to ragged simple rows instead of forcing rectangular output', () => {
    assert.deepStrictEqual(parseWritten(Buffer.from('a,b\n1\n2,3,4\n')), [
      ['a', 'b'],
      ['1'],
      ['2', '3', '4'],
    ]);
  });

  it('falls back to native parsing for invalid UTF-8 bytes', () => {
    assert.deepStrictEqual(parseWritten(Buffer.from([0x61, 0x2c, 0xff, 0x0a])), [
      ['a', '\ufffd'],
    ]);
  });
});
