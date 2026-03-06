# cisv-nodejs
[![CI](https://github.com/Sanix-Darker/cisv-nodejs/actions/workflows/ci.yml/badge.svg)](https://github.com/Sanix-Darker/cisv-nodejs/actions/workflows/ci.yml)
[![npm version](https://img.shields.io/npm/v/cisv.svg)](https://www.npmjs.com/package/cisv)
[npm Package](https://www.npmjs.com/package/cisv)

![License](https://img.shields.io/badge/license-MIT-blue)

Node.js binding distribution for CISV with a native Node-API addon backed by `cisv-core`.

## Features

- Native parser with SIMD-accelerated core
- Sync and async parsing APIs
- Streaming and chunked input support
- Iterator API for low-memory large-file processing
- Transform pipeline (C transforms + JavaScript transforms)

## Installation

### From npm

```bash
npm install cisv
```

### From source

```bash
git clone --recurse-submodules https://github.com/Sanix-Darker/cisv-nodejs
cd cisv-nodejs
make -C core/core all
cd bindings/nodejs
npm ci
npm run build
```

## Core Dependency (Submodule)

This repository tracks `cisv-core` via the `./core` git submodule.

To fetch the latest `cisv-core` (main branch) in your local clone:

```bash
git submodule update --init --remote --recursive
```

CI and release workflows also run this update command, so new `cisv-core` releases are pulled automatically during builds.

## Quick Start

```javascript
const { cisvParser } = require("cisv");

const parser = new cisvParser({ delimiter: ",", trim: true });
const rows = parser.parseSync("data.csv");
console.log(rows[0]);
```

## API Examples

### Async parse

```javascript
const { cisvParser } = require("cisv");

(async () => {
  const parser = new cisvParser();
  const rows = await parser.parse("data.csv");
  console.log(rows.length);
})();
```

### Parse from string

```javascript
const { cisvParser } = require("cisv");

const parser = new cisvParser();
const rows = parser.parseString("id,name\n1,alice\n2,bob");
```

### Iterator mode (large files)

```javascript
const { cisvParser } = require("cisv");

const parser = new cisvParser({ trim: true });
parser.openIterator("very_large.csv");

let row;
while ((row = parser.fetchRow()) !== null) {
  if (row[0] === "STOP") break;
}

parser.closeIterator();
```

### Transform by header name

```javascript
const { cisvParser } = require("cisv");

const parser = new cisvParser();
parser.setHeaderFields(["id", "name", "email"]);
parser.transformByName("name", "uppercase");

const rows = parser.parseString("id,name,email\n1,john,john@example.com");
console.log(rows[1][1]); // JOHN
```

## Examples Directory

Runnable examples are available in [`examples/`](./examples):

- `basic.js`
- `iterator.js`
- `sample.csv`

## Testing

```bash
cd bindings/nodejs
npm test
```

## Benchmarks

```bash
docker build -t cisv-node-bench -f bindings/nodejs/benchmarks/Dockerfile .
docker run --rm --platform linux/amd64 --cpus=2 --memory=4g cisv-node-bench
```

The benchmark output includes both full parse and iterator paths (including `cisv-iterator`).

## Upstream Core

- cisv-core: https://github.com/Sanix-Darker/cisv-core
