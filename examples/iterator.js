const { cisvParser } = require('../bindings/nodejs/cisv');

const parser = new cisvParser({ delimiter: ',' });
parser.openIterator(__dirname + '/sample.csv');

let row;
while ((row = parser.fetchRow()) !== null) {
  console.log(row);
  if (row[0] === '2') break;
}

parser.closeIterator();
