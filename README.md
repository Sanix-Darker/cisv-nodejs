# cisv-nodejs

![License](https://img.shields.io/badge/license-MIT-blue)

Node.js binding distribution for CISV.

## Features

- Native addon backed by `cisv-core`
- Sync/streaming APIs
- Iterator API (`openIterator`/`fetchRow`) for low memory usage
- Transform pipeline support

## Installation

```bash
git clone https://github.com/Sanix-Darker/cisv-nodejs
cd cisv-nodejs
make -C core all
cd bindings/nodejs
npm ci
npm run build
```

## Node.js API

### Basic example

```javascript
const { cisvParser } = require('cisv');
const parser = new cisvParser({ delimiter: ',', trim: true });
const rows = parser.parseSync('data.csv');
```

### Detailed example (iterator + transforms)

```javascript
const parser = new cisvParser({ delimiter: ',' });
parser.transform(1, 'uppercase');
parser.openIterator('large.csv');
let row;
while ((row = parser.fetchRow()) !== null) {
  if (row[0] === 'stop') break;
}
parser.closeIterator();
```

More runnable examples: [`examples/`](./examples)

## Tests

```bash
cd bindings/nodejs
npm test
```

## Benchmarks

```bash
docker build -t cisv-node-bench -f bindings/nodejs/benchmarks/Dockerfile .
docker run --rm --platform linux/amd64 --cpus=2 --memory=4g cisv-node-bench
```

## Upstream Core

- cisv-core: https://github.com/Sanix-Darker/cisv-core
