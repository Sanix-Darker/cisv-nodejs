const { cisvParser } = require('../bindings/nodejs/cisv');

const parser = new cisvParser({ delimiter: ',', trim: true });
const rows = parser.parseSync(__dirname + '/sample.csv');
console.log(rows);
