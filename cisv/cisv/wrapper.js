'use strict';

const { isAscii } = require('buffer');

function fastConfigFromOptions(options) {
  if (options == null) {
    return { delimiter: ',', quote: '"' };
  }
  if (typeof options !== 'object') {
    return null;
  }

  const delimiter = options.delimiter == null ? ',' : options.delimiter;
  const quote = options.quote == null ? '"' : options.quote;
  if (typeof delimiter !== 'string' || delimiter.length !== 1) {
    return null;
  }
  if (typeof quote !== 'string' || quote.length !== 1) {
    return null;
  }

  if (options.escape != null && options.escape !== '') {
    return null;
  }
  if (options.comment != null && options.comment !== '') {
    return null;
  }
  if (options.trim || options.skipEmptyLines || options.relaxed || options.skipLinesWithError) {
    return null;
  }
  if (options.maxRowSize != null && options.maxRowSize !== 0) {
    return null;
  }

  const fromLine = options.fromLine == null ? 1 : options.fromLine;
  const toLine = options.toLine == null ? 0 : options.toLine;
  if (fromLine !== 0 && fromLine !== 1) {
    return null;
  }
  if (toLine !== 0) {
    return null;
  }

  return { delimiter, quote };
}

function chunkToLatin1String(chunk) {
  return Buffer.isBuffer(chunk) ? chunk.toString('latin1') : chunk;
}

function chunksToLatin1String(chunks) {
  if (chunks.length === 1) {
    return chunkToLatin1String(chunks[0]);
  }

  let out = '';
  for (let i = 0; i < chunks.length; i++) {
    out += chunkToLatin1String(chunks[i]);
  }
  return out;
}

function isSimpleAsciiLf(data, quote) {
  if (data.length >= 3 &&
      data.charCodeAt(0) === 0xEF &&
      data.charCodeAt(1) === 0xBB &&
      data.charCodeAt(2) === 0xBF) {
    return false;
  }

  const quoteCode = quote.charCodeAt(0);
  for (let i = 0; i < data.length; i++) {
    const code = data.charCodeAt(i);
    if (code === quoteCode || code === 13 || code > 127) {
      return false;
    }
  }
  return true;
}

function chunksAreSimpleAsciiLf(chunks, quote) {
  const quoteCode = quote.charCodeAt(0);
  for (let i = 0; i < chunks.length; i++) {
    const chunk = chunks[i];
    if (Buffer.isBuffer(chunk)) {
      if (i === 0 &&
          chunk.length >= 3 &&
          chunk[0] === 0xEF &&
          chunk[1] === 0xBB &&
          chunk[2] === 0xBF) {
        return false;
      }
      if (!isAscii(chunk) || chunk.indexOf(quoteCode) !== -1 || chunk.indexOf(13) !== -1) {
        return false;
      }
    } else if (!isSimpleAsciiLf(chunk, quote)) {
      return false;
    }
  }
  return true;
}

function analyzeSingleSimpleChunk(chunk, delimiter, quote) {
  const delimiterCode = delimiter.charCodeAt(0);
  const quoteCode = quote.charCodeAt(0);
  let cols = -1;
  let currentCols = 1;
  let hasData = false;
  let rows = 0;

  if (Buffer.isBuffer(chunk)) {
    if (chunk.length >= 3 && chunk[0] === 0xEF && chunk[1] === 0xBB && chunk[2] === 0xBF) {
      return { simple: false, uniform: false, rows: 0, cols: 0 };
    }
    for (let i = 0; i < chunk.length; i++) {
      const code = chunk[i];
      if (code === quoteCode || code === 13 || code > 127) {
        return { simple: false, uniform: false, rows: 0, cols: 0 };
      }
      if (code === delimiterCode) {
        currentCols++;
        hasData = true;
      } else if (code === 10) {
        if (cols === -1) {
          cols = currentCols;
        } else if (currentCols !== cols) {
          return { simple: true, uniform: false, rows: 0, cols: 0 };
        }
        rows++;
        currentCols = 1;
        hasData = false;
      } else {
        hasData = true;
      }
    }
  } else {
    if (chunk.length >= 3 &&
        chunk.charCodeAt(0) === 0xEF &&
        chunk.charCodeAt(1) === 0xBB &&
        chunk.charCodeAt(2) === 0xBF) {
      return { simple: false, uniform: false, rows: 0, cols: 0 };
    }
    for (let i = 0; i < chunk.length; i++) {
      const code = chunk.charCodeAt(i);
      if (code === quoteCode || code === 13 || code > 127) {
        return { simple: false, uniform: false, rows: 0, cols: 0 };
      }
      if (code === delimiterCode) {
        currentCols++;
        hasData = true;
      } else if (code === 10) {
        if (cols === -1) {
          cols = currentCols;
        } else if (currentCols !== cols) {
          return { simple: true, uniform: false, rows: 0, cols: 0 };
        }
        rows++;
        currentCols = 1;
        hasData = false;
      } else {
        hasData = true;
      }
    }
  }

  if (hasData && cols !== -1 && currentCols !== cols) {
    return { simple: true, uniform: false, rows: 0, cols: 0 };
  }
  if (hasData) {
    rows++;
    if (cols === -1) {
      cols = currentCols;
    }
  }

  return { simple: true, uniform: true, rows, cols: Math.max(cols, 0) };
}

function parseSimpleRows(data, delimiter) {
  if (data.length === 0) {
    return [];
  }

  const last = data.charCodeAt(data.length - 1);
  const body = last === 10 ? data.slice(0, -1) : data;
  if (body.length === 0) {
    return [['']];
  }

  const lines = body.split('\n');
  const rows = new Array(lines.length);
  for (let i = 0; i < lines.length; i++) {
    rows[i] = lines[i].split(delimiter);
  }
  return rows;
}

function parseUniformRows(data, delimiter, rowCount, cols) {
  let end = data.length;
  if (end === 0) {
    return [];
  }
  if (data.charCodeAt(end - 1) === 10) {
    end--;
  }
  if (end === 0) {
    return [['']];
  }

  const usePrealloc = rowCount > 0 && cols > 0;
  if (!usePrealloc) {
    cols = 1;
    for (let i = 0; i < end && data.charCodeAt(i) !== 10; i++) {
      if (data[i] === delimiter) {
        cols++;
      }
    }
  }

  const rows = usePrealloc ? new Array(rowCount) : [];
  let rowIdx = 0;
  let pos = 0;
  while (pos < end) {
    const row = new Array(cols);
    for (let col = 0; col < cols - 1; col++) {
      const next = data.indexOf(delimiter, pos);
      row[col] = data.slice(pos, next);
      pos = next + 1;
    }

    let lineEnd = data.indexOf('\n', pos);
    if (lineEnd === -1 || lineEnd > end) {
      lineEnd = end;
    }
    row[cols - 1] = data.slice(pos, lineEnd);
    if (usePrealloc) {
      rows[rowIdx++] = row;
    } else {
      rows.push(row);
    }
    pos = lineEnd + 1;
  }

  return rows;
}

function wrapAddon(addon) {
  const NativeParser = addon.cisvParser;

  class cisvParser extends NativeParser {
    constructor(options) {
      super(options);
      this._cisvFastConfig = fastConfigFromOptions(options);
      this._cisvFastChunks = [];
      this._cisvFastRows = null;
      this._cisvNativeStream = false;
    }

    _flushFastChunksToNative() {
      if (this._cisvFastChunks.length === 0) {
        return;
      }
      const chunks = this._cisvFastChunks;
      this._cisvFastChunks = [];
      this._cisvNativeStream = true;
      for (let i = 0; i < chunks.length; i++) {
        super.write(chunks[i]);
      }
    }

    write(chunk) {
      this._cisvFastRows = null;
      if (this._cisvFastConfig &&
          !this._cisvNativeStream &&
          (Buffer.isBuffer(chunk) || typeof chunk === 'string')) {
        this._cisvFastChunks.push(chunk);
        return;
      }

      this._flushFastChunksToNative();
      this._cisvNativeStream = true;
      return super.write(chunk);
    }

    end() {
      if (this._cisvFastConfig &&
          !this._cisvNativeStream &&
          this._cisvFastChunks.length > 0) {
        let uniform = false;
        let simple = false;

        if (this._cisvFastChunks.length === 1) {
          const analysis = analyzeSingleSimpleChunk(
            this._cisvFastChunks[0],
            this._cisvFastConfig.delimiter,
            this._cisvFastConfig.quote);
          simple = analysis.simple;
          uniform = analysis.uniform;
          var rowCount = analysis.rows;
          var colCount = analysis.cols;
        } else {
          simple = chunksAreSimpleAsciiLf(this._cisvFastChunks, this._cisvFastConfig.quote);
        }

        if (simple) {
          const data = chunksToLatin1String(this._cisvFastChunks);
          const useLargePrealloc = data.length >= 64 * 1024 * 1024;
          this._cisvFastRows = uniform
            ? parseUniformRows(
                data,
                this._cisvFastConfig.delimiter,
                useLargePrealloc ? rowCount : 0,
                useLargePrealloc ? colCount : 0)
            : parseSimpleRows(data, this._cisvFastConfig.delimiter);
          this._cisvFastChunks = [];
          return;
        }
      }

      this._flushFastChunksToNative();
      this._cisvNativeStream = true;
      return super.end();
    }

    getRows() {
      if (this._cisvFastRows !== null) {
        return this._cisvFastRows;
      }
      return super.getRows();
    }

    clear() {
      this._cisvFastChunks = [];
      this._cisvFastRows = null;
      this._cisvNativeStream = false;
      return super.clear();
    }

    setConfig(options) {
      this._flushFastChunksToNative();
      this._cisvFastRows = null;
      this._cisvFastConfig = fastConfigFromOptions(options);
      return super.setConfig(options);
    }

    transform(...args) {
      this._flushFastChunksToNative();
      this._cisvFastConfig = null;
      return super.transform(...args);
    }

    transformByName(...args) {
      this._flushFastChunksToNative();
      this._cisvFastConfig = null;
      return super.transformByName(...args);
    }

    destroy() {
      this._cisvFastChunks = [];
      this._cisvFastRows = null;
      return super.destroy();
    }
  }

  return {
    ...addon,
    cisvParser,
  };
}

module.exports = { wrapAddon };
