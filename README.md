# cisv-nodejs

Node.js binding distribution for CISV.

## Upstream core

- cisv-core: https://github.com/Sanix-Darker/cisv-core

## Build

```bash
make all
```

## Test

```bash
cd bindings/nodejs && npm test
```

## Benchmark Docker

```bash
docker build -t cisv-node-bench -f bindings/nodejs/benchmarks/Dockerfile .
docker run --rm --platform linux/amd64 --cpus=2 --memory=4g cisv-node-bench
```
