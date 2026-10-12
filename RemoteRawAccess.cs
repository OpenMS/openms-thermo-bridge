// EXPERIMENTAL: read Thermo RAW files over HTTP(S) / S3 (or a local file via the same path)
// through the RawFileReader "delegated random access" plug-in interface.
//
// The vendor loader (RandomAccessRawFileLoader) asks an IViewCollectionManager for
// IReadWriteAccessor views at (offset, size). Thermo's own implementation
// (RandomAccessReaderPlugin.RandomAccessFileManager) reads a local file handle; this one
// serves the same calls from a block cache filled by ranged reads of a RangeSource.
// Accessor semantics mirror the vendor plug-in (offsets relative to the view's
// InitialOffset, Length = whole file, sub-views < 2 MB copied into a MemoryArrayAccessor),
// except PreferLargeReads = false by default: like the memory-mapped reader used for normal
// local opens, the vendor then reads fields one by one (served from the cache) instead of
// fixed 1 MB / multi-MB batches, which over-fetch badly on random access.
//
// Environment (all optional):
//   THERMO_BRIDGE_REMOTE_BLOCK_KB     cache block size in KiB (default 64): the fetch unit for
//                                     isolated random misses
//   THERMO_BRIDGE_REMOTE_REGION_KB    region size in KiB (default 2048; 0 disables): after
//   THERMO_BRIDGE_REMOTE_REGION_MISSES  this many (default 4) non-sequential misses in a region,
//                                     the next one fetches all its missing blocks at once, so
//                                     dense regions need few requests while sparse random access
//                                     transfers little
//   THERMO_BRIDGE_REMOTE_CACHE_MB     cache capacity in MiB per file (default 256)
//   THERMO_BRIDGE_REMOTE_READAHEAD_KB max readahead window in KiB for sequential streams (default 8192)
//   THERMO_BRIDGE_REMOTE_PARALLEL     max concurrent range requests per multi-block fetch (default 4)
//   THERMO_BRIDGE_REMOTE_LARGE_READS  "1": PreferLargeReads = true (vendor plug-in behaviour)
//   THERMO_BRIDGE_REMOTE_CHUNK_KB     with LARGE_READS=1: vendor record batch size in KiB
//                                     (default: plug-in heuristic, 1-8 MB)
//   THERMO_BRIDGE_REMOTE_STATS        "1": print request/byte counters to stderr on close
//   THERMO_BRIDGE_REMOTE_TRACE        "1": print every range request with its stack to stderr
//   THERMO_BRIDGE_FORCE_RANGE_READER  "1": open local files through this path too (testing)
//   THERMO_BRIDGE_FORCE_VENDOR_PLUGIN "1": open local files with Thermo's plug-in (diagnostics)
//   THERMO_BRIDGE_S3_ANONYMOUS        "1": do not sign S3 requests
//   AWS_ENDPOINT_URL[_S3], AWS_REGION and the standard AWS credential chain for s3:// URLs.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;
using ThermoFisher.CommonCore.RawFileReader.Facade.Interfaces;
using ThermoFisher.CommonCore.RawFileReader.Readers;

namespace ThermoWrapperManaged.Remote
{
    // ----------------------------------------------------------------
    // Byte sources
    // ----------------------------------------------------------------
    internal abstract class RangeSource : IDisposable
    {
        private long _requests;
        private long _bytes;

        public abstract long Length { get; }
        public abstract string Description { get; }
        /// Whether concurrent Read calls are worthwhile (network sources).
        public virtual bool SupportsParallel => true;
        public long Requests => Interlocked.Read(ref _requests);
        public long BytesFetched => Interlocked.Read(ref _bytes);

        /// Fill dst completely from [offset, offset + dst.Length); the range must lie inside the file.
        protected abstract void ReadCore(long offset, Span<byte> dst);

        private static readonly bool Trace = Environment.GetEnvironmentVariable("THERMO_BRIDGE_REMOTE_TRACE") == "1";

        public void Read(long offset, Span<byte> dst)
        {
            if (dst.Length == 0) return;
            if (Trace) Console.Error.WriteLine($"[thermo-bridge remote] GET {offset}+{dst.Length}\n{Environment.StackTrace}");
            Interlocked.Increment(ref _requests);
            Interlocked.Add(ref _bytes, dst.Length);
            ReadCore(offset, dst);
        }

        public virtual void Dispose() { }

        public static bool IsRemote(string path) =>
            path.StartsWith("http://", StringComparison.OrdinalIgnoreCase)
            || path.StartsWith("https://", StringComparison.OrdinalIgnoreCase)
            || path.StartsWith("s3://", StringComparison.OrdinalIgnoreCase);

        public static RangeSource Create(string path)
        {
            if (path.StartsWith("s3://", StringComparison.OrdinalIgnoreCase))
            {
#if THERMO_BRIDGE_S3
                return new S3RangeSource(path);
#else
                return new HttpRangeSource(S3ToHttps(path));
#endif
            }
            if (IsRemote(path)) return new HttpRangeSource(path);
            return new LocalFileRangeSource(path);
        }

        /// Unsigned s3:// access without the AWS SDK: public objects only.
        internal static string S3ToHttps(string s3Url)
        {
            var rest = s3Url.Substring("s3://".Length);
            int slash = rest.IndexOf('/');
            if (slash <= 0) throw new ArgumentException("expected s3://bucket/key", nameof(s3Url));
            string bucket = rest.Substring(0, slash), key = rest.Substring(slash + 1);
            string escapedKey = string.Join("/", Array.ConvertAll(key.Split('/'), Uri.EscapeDataString));
            var endpoint = Environment.GetEnvironmentVariable("AWS_ENDPOINT_URL_S3")
                           ?? Environment.GetEnvironmentVariable("AWS_ENDPOINT_URL");
            if (!string.IsNullOrEmpty(endpoint)) return endpoint.TrimEnd('/') + "/" + bucket + "/" + escapedKey;
            var region = Environment.GetEnvironmentVariable("AWS_REGION")
                         ?? Environment.GetEnvironmentVariable("AWS_DEFAULT_REGION");
            return string.IsNullOrEmpty(region)
                ? $"https://{bucket}.s3.amazonaws.com/{escapedKey}"
                : $"https://{bucket}.s3.{region}.amazonaws.com/{escapedKey}";
        }
    }

    internal sealed class LocalFileRangeSource : RangeSource
    {
        private readonly SafeFileHandle _handle;
        private readonly string _path;

        public LocalFileRangeSource(string path)
        {
            _path = path;
            _handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            Length = RandomAccess.GetLength(_handle);
        }

        public override long Length { get; }
        public override string Description => _path;
        public override bool SupportsParallel => false;

        protected override void ReadCore(long offset, Span<byte> dst)
        {
            while (dst.Length > 0)
            {
                int n = RandomAccess.Read(_handle, dst, offset);
                if (n <= 0) throw new EndOfStreamException($"short read at {offset} in {_path}");
                dst = dst.Slice(n);
                offset += n;
            }
        }

        public override void Dispose() => _handle.Dispose();
    }

    internal sealed class HttpRangeSource : RangeSource
    {
        private static readonly HttpClient Client = new(new SocketsHttpHandler
        {
            AutomaticDecompression = DecompressionMethods.None,
            MaxConnectionsPerServer = 32,
            PooledConnectionLifetime = TimeSpan.FromMinutes(5),
        })
        { Timeout = TimeSpan.FromMinutes(5) };

        private readonly Uri _uri;

        public HttpRangeSource(string url)
        {
            _uri = new Uri(url);
            // A one-byte range request both checks Range support and returns the total size.
            using var response = Send(0, 1);
            var range = response.Content.Headers.ContentRange;
            if (response.StatusCode != HttpStatusCode.PartialContent || range?.Length == null)
                throw new NotSupportedException($"server does not support byte ranges ({(int)response.StatusCode}) for {Description}");
            Length = range.Length.Value;
        }

        public override long Length { get; }
        // Never expose the query string: presigned URLs carry credentials there.
        public override string Description => _uri.GetLeftPart(UriPartial.Path);

        private HttpResponseMessage Send(long offset, int count)
        {
            for (int attempt = 0; ; attempt++)
            {
                HttpResponseMessage? response = null;
                try
                {
                    using var request = new HttpRequestMessage(HttpMethod.Get, _uri);
                    request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(offset, offset + count - 1);
                    response = Client.Send(request, HttpCompletionOption.ResponseHeadersRead);
                    if ((int)response.StatusCode >= 500 && attempt < 3) { response.Dispose(); Backoff(attempt); continue; }
                    if (!response.IsSuccessStatusCode)
                        throw new IOException($"HTTP {(int)response.StatusCode} for {Description}");
                    return response;
                }
                catch (Exception e) when (attempt < 3 && (e is HttpRequestException || e is System.Threading.Tasks.TaskCanceledException))
                {
                    response?.Dispose();
                    Backoff(attempt);
                }
            }
        }

        private static void Backoff(int attempt) => Thread.Sleep(100 * (1 << attempt));

        protected override void ReadCore(long offset, Span<byte> dst)
        {
            for (int attempt = 0; ; attempt++)
            {
                try
                {
                    using var response = Send(offset, dst.Length);
                    var range = response.Content.Headers.ContentRange;
                    if (response.StatusCode != HttpStatusCode.PartialContent || range?.From != offset)
                        throw new IOException($"unexpected range response for {Description}");
                    using var stream = response.Content.ReadAsStream();
                    var rest = dst;
                    while (rest.Length > 0)
                    {
                        int n = stream.Read(rest);
                        if (n <= 0) throw new IOException($"truncated range response for {Description}");
                        rest = rest.Slice(n);
                    }
                    return;
                }
                catch (IOException) when (attempt < 3)
                {
                    Backoff(attempt);
                }
            }
        }
    }

#if THERMO_BRIDGE_S3
    internal sealed class S3RangeSource : RangeSource
    {
        private readonly Amazon.S3.AmazonS3Client _client;
        private readonly string _bucket;
        private readonly string _key;

        public S3RangeSource(string s3Url)
        {
            var rest = s3Url.Substring("s3://".Length);
            int slash = rest.IndexOf('/');
            if (slash <= 0) throw new ArgumentException("expected s3://bucket/key", nameof(s3Url));
            _bucket = rest.Substring(0, slash);
            _key = rest.Substring(slash + 1);

            var config = new Amazon.S3.AmazonS3Config();
            var endpoint = Environment.GetEnvironmentVariable("AWS_ENDPOINT_URL_S3")
                           ?? Environment.GetEnvironmentVariable("AWS_ENDPOINT_URL");
            if (!string.IsNullOrEmpty(endpoint))
            {
                config.ServiceURL = endpoint;
                config.ForcePathStyle = true;
            }
            var region = Environment.GetEnvironmentVariable("AWS_REGION")
                         ?? Environment.GetEnvironmentVariable("AWS_DEFAULT_REGION");
            if (!string.IsNullOrEmpty(region) && string.IsNullOrEmpty(endpoint))
                config.RegionEndpoint = Amazon.RegionEndpoint.GetBySystemName(region);
            else if (string.IsNullOrEmpty(endpoint))
                config.RegionEndpoint = Amazon.RegionEndpoint.USEast1;

            Amazon.Runtime.AWSCredentials credentials;
            if (Environment.GetEnvironmentVariable("THERMO_BRIDGE_S3_ANONYMOUS") == "1")
                credentials = new Amazon.Runtime.AnonymousAWSCredentials();
            else
            {
                try { credentials = Amazon.Runtime.FallbackCredentialsFactory.GetCredentials(); }
                catch (Amazon.Runtime.AmazonClientException) { credentials = new Amazon.Runtime.AnonymousAWSCredentials(); }
            }
            _client = new Amazon.S3.AmazonS3Client(credentials, config);
            var meta = _client.GetObjectMetadataAsync(_bucket, _key).GetAwaiter().GetResult();
            Length = meta.ContentLength;
        }

        public override long Length { get; }
        public override string Description => $"s3://{_bucket}/{_key}";

        protected override void ReadCore(long offset, Span<byte> dst)
        {
            var request = new Amazon.S3.Model.GetObjectRequest
            {
                BucketName = _bucket,
                Key = _key,
                ByteRange = new Amazon.S3.Model.ByteRange(offset, offset + dst.Length - 1),
            };
            using var response = _client.GetObjectAsync(request).GetAwaiter().GetResult();
            using var stream = response.ResponseStream;
            while (dst.Length > 0)
            {
                int n = stream.Read(dst);
                if (n <= 0) throw new IOException($"truncated S3 range response for {Description}");
                dst = dst.Slice(n);
            }
        }

        public override void Dispose() => _client.Dispose();
    }
#endif

    // ----------------------------------------------------------------
    // Block cache: aligned blocks, LRU, thread-safe, one fetch per block.
    // Contiguous missing blocks of one read are fetched with a single request; a miss
    // right after the previous block extends the request by up to `readahead` blocks.
    // ----------------------------------------------------------------
    internal sealed class BlockCache
    {
        /// Immutable view of a loaded block; accessors keep the last one for lock-free re-reads.
        internal sealed class Hot
        {
            public readonly long Start;
            public readonly byte[] Data;
            public Hot(long start, byte[] data) { Start = start; Data = data; }
        }

        private sealed class Block
        {
            public byte[]? Data;
            public Hot? View;
            public Exception? Error;
            public long LastUse;
            public readonly ManualResetEventSlim Ready = new(false);
        }

        private readonly RangeSource _source;
        private readonly int _blockSize;
        private readonly int _maxBlocks;
        private readonly int _readahead;
        // Lock-free lookups on the hit path; inserts, eviction and failure cleanup take _lock.
        // Eviction is approximate LRU on a per-block use stamp (blocks are few, eviction is rare).
        private readonly ConcurrentDictionary<long, Block> _blocks = new();
        private readonly object _lock = new();
        private long _hits, _misses, _clock;
        private readonly long[] _streamNext = { -1, -1, -1, -1, -1, -1, -1, -1 };
        private readonly long[] _streamUse = new long[8];
        private readonly int[] _streamWindow = new int[8];

        private readonly int _parallel;

        private readonly int _regionBlocks, _regionThreshold;
        private readonly Dictionary<long, int> _regionMisses = new();

        public BlockCache(RangeSource source, int blockSize, long capacityBytes, int readahead, int parallel, int regionBlocks = 1, int regionThreshold = 2)
        {
            _regionThreshold = Math.Max(1, regionThreshold);
            _regionBlocks = Math.Max(1, regionBlocks);
            _parallel = Math.Max(1, parallel);
            _source = source;
            _blockSize = Math.Max(4096, blockSize);
            _maxBlocks = (int)Math.Max(2, capacityBytes / _blockSize);
            _readahead = Math.Max(0, readahead);
        }

        public long Length => _source.Length;
        public int BlockSize => _blockSize;
        public long Hits => Interlocked.Read(ref _hits);
        public long Misses => Interlocked.Read(ref _misses);

        /// Copy [offset, offset + dst.Length) into dst; bytes past the end of the file are zeroed
        /// (same as the vendor plug-in's short RandomAccess.Read).
        public void Read(long offset, Span<byte> dst) => Read(offset, dst, out _);

        /// As Read; also returns the last block touched (null if none) for RangeAccessor's fast path.
        public void Read(long offset, Span<byte> dst, out Hot? hot)
        {
            hot = null;
            if (offset < 0) throw new ArgumentOutOfRangeException(nameof(offset));
            long end = Math.Min(offset + dst.Length, Length);
            if (end < offset + dst.Length) dst.Slice((int)Math.Max(0, end - offset)).Clear();
            if (end <= offset) return;
            int count = (int)(end - offset);

            // Reads larger than a quarter of the cache go straight to the source.
            if ((long)count * 4 > (long)_maxBlocks * _blockSize)
            {
                _source.Read(offset, dst.Slice(0, count));
                return;
            }

            long first = offset / _blockSize, last = (end - 1) / _blockSize;
            for (long b = first; b <= last; b++)
            {
                if (!_blocks.TryGetValue(b, out var block) || !block.Ready.IsSet || block.Error != null)
                    block = Acquire(b, last);
                else
                    Interlocked.Increment(ref _hits);
                // Plain stamp from the miss clock: cheap approximate LRU without a contended counter.
                block.LastUse = Volatile.Read(ref _clock);
                block.Ready.Wait();
                if (block.Error != null) throw new IOException("remote read failed", block.Error);
                long blockStart = b * _blockSize;
                long from = Math.Max(offset, blockStart), to = Math.Min(end, blockStart + block.Data!.Length);
                block.Data.AsSpan((int)(from - blockStart), (int)(to - from)).CopyTo(dst.Slice((int)(from - offset)));
                hot = block.View;
            }
        }

        /// Slow path for block b: start one coalesced fetch for the missing blocks in [b, last]
        /// (plus sequential readahead) and return block b, which may still be in flight.
        private Block Acquire(long b, long last)
        {
            Block result;
            long runStart = -1;
            List<Block>? run = null;
            lock (_lock)
            {
                if (_blocks.TryGetValue(b, out var existing) && existing.Error == null)
                {
                    Interlocked.Increment(ref _hits);
                    return existing;
                }
                long lastBlock = (Length - 1) / _blockSize;
                result = AddBlock(b);
                runStart = b;
                run = new List<Block> { result };
                Interlocked.Increment(ref _misses);
                long next = b + 1;
                for (; next <= last && !_blocks.ContainsKey(next); next++)
                {
                    run.Add(AddBlock(next));
                    Interlocked.Increment(ref _misses);
                }
                // Sequential readahead: the vendor reads several regions in interleaved sequential
                // streams (scan index, packets, trailers), so track a few stream cursors. A miss that
                // starts where a stream's last fetch ended extends that stream by a window that
                // doubles per miss (up to _readahead blocks); other misses start a new stream with
                // no readahead, so random access fetches only the blocks it touches.
                int stream = Array.IndexOf(_streamNext, b);
                if (stream < 0)
                {
                    stream = 0;
                    for (int i = 1; i < _streamNext.Length; i++)
                        if (_streamUse[i] < _streamUse[stream]) stream = i;
                    _streamWindow[stream] = 0;
                    // Region promotion: an isolated miss fetches only its block; a repeated miss in
                    // the same region fetches the region's contiguous missing blocks around it.
                    if (_regionBlocks > 1)
                    {
                        long region = b / _regionBlocks;
                        _regionMisses.TryGetValue(region, out int seen);
                        _regionMisses[region] = seen + 1;
                        if (seen + 1 >= _regionThreshold)
                        {
                            long lo = region * _regionBlocks, hi = Math.Min(lastBlock, lo + _regionBlocks - 1);
                            for (; next <= hi && !_blocks.ContainsKey(next); next++)
                            {
                                run.Add(AddBlock(next));
                                Interlocked.Increment(ref _misses);
                            }
                            while (runStart - 1 >= lo && !_blocks.ContainsKey(runStart - 1))
                            {
                                run.Insert(0, AddBlock(--runStart));
                                Interlocked.Increment(ref _misses);
                            }
                        }
                    }
                }
                else if (next > last && _readahead > 0)
                {
                    _streamWindow[stream] = Math.Min(_readahead, Math.Max(1, _streamWindow[stream] * 2));
                    for (; next <= Math.Min(lastBlock, last + _streamWindow[stream]) && !_blocks.ContainsKey(next); next++)
                        run.Add(AddBlock(next));
                }
                _streamNext[stream] = next;
                _streamUse[stream] = Interlocked.Increment(ref _clock);
                Evict();
            }
            Fetch(runStart, run);
            return result;
        }

        private Block AddBlock(long index)
        {
            var block = new Block { LastUse = Interlocked.Increment(ref _clock) };
            _blocks[index] = block;
            return block;
        }

        private void Evict()
        {
            int excess = _blocks.Count - _maxBlocks;
            if (excess <= 0) return;
            var victims = new List<KeyValuePair<long, Block>>();
            foreach (var kv in _blocks)
                if (kv.Value.Ready.IsSet) victims.Add(kv);
            victims.Sort((x, y) => x.Value.LastUse.CompareTo(y.Value.LastUse));
            for (int i = 0; i < Math.Min(excess, victims.Count); i++) _blocks.TryRemove(victims[i]);
        }

        private void Fetch(long startBlock, List<Block> blocks)
        {
            try
            {
                // Long runs are split into up to _parallel concurrent range requests: a single
                // HTTP/S3 stream is usually bandwidth-limited well below the link speed.
                int parts = Math.Min(_parallel, blocks.Count);
                if (parts <= 1 || !_source.SupportsParallel)
                {
                    FetchRun(startBlock, blocks, 0, blocks.Count);
                }
                else
                {
                    int per = (blocks.Count + parts - 1) / parts;
                    System.Threading.Tasks.Parallel.For(0, parts, new System.Threading.Tasks.ParallelOptions { MaxDegreeOfParallelism = parts },
                        p => FetchRun(startBlock, blocks, p * per, Math.Min(blocks.Count, (p + 1) * per)));
                }
            }
            catch (Exception e)
            {
                if (e is AggregateException agg && agg.InnerExceptions.Count == 1) e = agg.InnerExceptions[0];
                foreach (var b in blocks) b.Error = e;
                lock (_lock)
                {
                    // Drop failed blocks so a later read retries them.
                    for (int i = 0; i < blocks.Count; i++)
                        _blocks.TryRemove(new KeyValuePair<long, Block>(startBlock + i, blocks[i]));
                }
            }
            finally
            {
                foreach (var b in blocks) b.Ready.Set();
            }
        }

        private void FetchRun(long startBlock, List<Block> blocks, int from, int to)
        {
            if (from >= to) return;
            long start = (startBlock + from) * _blockSize;
            long end = Math.Min(Length, (startBlock + to) * _blockSize);
            var buffer = new byte[end - start];
            _source.Read(start, buffer);
            for (int i = from; i < to; i++)
            {
                long bs = (long)(i - from) * _blockSize;
                int len = (int)Math.Min(_blockSize, buffer.Length - bs);
                var data = to - from == 1 ? buffer : buffer.AsSpan((int)bs, len).ToArray();
                blocks[i].Data = data;
                blocks[i].View = new Hot((startBlock + i) * _blockSize, data);
            }
        }
    }

    /// A RAW file served through the range cache. Owned by FilePool; disposed with the last handle.
    internal sealed class RemoteFile : IDisposable
    {
        public readonly string Name;
        public readonly RangeSource Source;
        public readonly BlockCache Cache;
        private readonly Stopwatch _clock = Stopwatch.StartNew();

        public RemoteFile(string name, RangeSource source)
        {
            Name = name;
            Source = source;
            int blockKb = EnvInt("THERMO_BRIDGE_REMOTE_BLOCK_KB", 64);
            var regionEnv = Environment.GetEnvironmentVariable("THERMO_BRIDGE_REMOTE_REGION_KB");
            int regionKb = int.TryParse(regionEnv, out var r) && r >= 0 ? r : 2048;
            int regionBlocks = Math.Max(1, regionKb / Math.Max(1, blockKb));
            int regionMisses = EnvInt("THERMO_BRIDGE_REMOTE_REGION_MISSES", 4);
            long cacheMb = EnvInt("THERMO_BRIDGE_REMOTE_CACHE_MB", 256);
            int readahead = (int)Math.Max(0, EnvInt("THERMO_BRIDGE_REMOTE_READAHEAD_KB", 8192) / Math.Max(1, blockKb));
            int parallel = EnvInt("THERMO_BRIDGE_REMOTE_PARALLEL", 4);
            Cache = new BlockCache(source, blockKb * 1024, cacheMb * 1024 * 1024, readahead, parallel, regionBlocks, regionMisses);
            ChunkSize = int.TryParse(Environment.GetEnvironmentVariable("THERMO_BRIDGE_REMOTE_CHUNK_KB"), out var chunkKb)
                ? (chunkKb < 0 ? -1 : chunkKb * 1024) : -1;
        }

        /// SuggestedChunkSize for accessors; -1 = vendor plug-in heuristic.
        public readonly int ChunkSize;
        /// PreferLargeReads for accessors.
        public readonly bool LargeReads = Environment.GetEnvironmentVariable("THERMO_BRIDGE_REMOTE_LARGE_READS") == "1";

        private static int EnvInt(string name, int fallback) =>
            int.TryParse(Environment.GetEnvironmentVariable(name), out var v) && v > 0 ? v : fallback;

        public string Stats() =>
            $"[thermo-bridge remote] {Source.Description}: size={Source.Length} block={Cache.BlockSize} "
            + $"requests={Source.Requests} bytes={Source.BytesFetched} ({100.0 * Source.BytesFetched / Math.Max(1, Source.Length):F1}% of file) "
            + $"hits={Cache.Hits} misses={Cache.Misses} open_for={_clock.Elapsed.TotalSeconds:F2}s";

        public void Dispose()
        {
            if (Environment.GetEnvironmentVariable("THERMO_BRIDGE_REMOTE_STATS") == "1") Console.Error.WriteLine(Stats());
            Source.Dispose();
        }

        /// Uncached sequential stream over the whole object (used for checksums).
        public Stream OpenSequentialStream() => new RangeSourceStream(Source);

        private sealed class RangeSourceStream : Stream
        {
            private readonly RangeSource _source;
            private long _position;
            public RangeSourceStream(RangeSource source) { _source = source; }
            public override bool CanRead => true;
            public override bool CanSeek => false;
            public override bool CanWrite => false;
            public override long Length => _source.Length;
            public override long Position { get => _position; set => throw new NotSupportedException(); }
            public override void Flush() { }
            public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));
            public override int Read(Span<byte> buffer)
            {
                int n = (int)Math.Min(buffer.Length, _source.Length - _position);
                if (n <= 0) return 0;
                _source.Read(_position, buffer.Slice(0, n));
                _position += n;
                return n;
            }
            public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
            public override void SetLength(long value) => throw new NotSupportedException();
            public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        }
    }

    // ----------------------------------------------------------------
    // IViewCollectionManager handed to RawFileReaderAdapter.RandomAccessThreadedFileFactory.
    // ----------------------------------------------------------------
    internal sealed class RemoteViewCollectionManager : IViewCollectionManager
    {
        public static readonly RemoteViewCollectionManager Instance = new();

        private readonly ConcurrentDictionary<string, RemoteFile> _files = new(StringComparer.Ordinal);
        private readonly ConcurrentDictionary<string, string> _errors = new(StringComparer.Ordinal);

        public Dictionary<string, string> ExtensionAttributes { get; } = new();

        // Remote names (URLs, S3 keys) are case sensitive: stop the reader from lower-casing them on Windows.
        public bool GetIgnorePlatformKeepNameCaseIntactFlag() => true;

        public void Register(RemoteFile file)
        {
            if (!_files.TryAdd(file.Name, file)) throw new InvalidOperationException("name already registered: " + file.Name);
        }

        public void Unregister(RemoteFile file)
        {
            _files.TryRemove(new KeyValuePair<string, RemoteFile>(file.Name, file));
            _errors.TryRemove(file.Name, out _);
        }

        public bool IsRegistered(string name) => _files.ContainsKey(name);

        /// Accepts a registered name or a stream id "<32 hex>__<name>" (the reader passes both).
        private RemoteFile? Resolve(string? nameOrStreamId)
        {
            if (string.IsNullOrEmpty(nameOrStreamId)) return null;
            if (_files.TryGetValue(nameOrStreamId, out var file)) return file;
            if (nameOrStreamId.Length > 34 && nameOrStreamId[32] == '_' && nameOrStreamId[33] == '_'
                && _files.TryGetValue(nameOrStreamId.Substring(34), out file)) return file;
            return null;
        }

        public void Close(string streamId, bool forceToClose = false) { }

        public bool IsOpen(string streamId) => Resolve(streamId) != null;

        public string GetErrors(string streamId)
        {
            var file = Resolve(streamId);
            return file != null && _errors.TryGetValue(file.Name, out var e) ? e : string.Empty;
        }

        public IReadWriteAccessor? GetRandomAccessViewer(Guid id, string fileName, bool inAcquisition,
            DataFileAccessMode accessMode = DataFileAccessMode.OpenCreateRead, PersistenceMode type = PersistenceMode.Persisted)
            => GetRandomAccessViewer(id, fileName, 0L, 0L, inAcquisition, accessMode, type);

        public IReadWriteAccessor? GetRandomAccessViewer(Guid loaderId, string dataName, long offset, long size, bool inAcquisition,
            DataFileAccessMode accessMode = DataFileAccessMode.OpenCreateRead, PersistenceMode type = PersistenceMode.Persisted)
        {
            string streamId = loaderId == Guid.Empty ? dataName : StreamHelper.ConstructStreamId(loaderId, dataName);
            if ((accessMode & DataFileAccessMode.Write) == DataFileAccessMode.Write)
                return new MemoryArrayAccessor(new byte[size], 0L, streamId);
            var file = Resolve(dataName);
            if (file == null) return null;
            bool permitMissingData = (accessMode & DataFileAccessMode.PermitMissingData) != 0;
            try
            {
                return new RangeAccessor(file, streamId, offset, size, permitMissingData);
            }
            catch (Exception e)
            {
                _errors[file.Name] = e.Message;
                return null;
            }
        }
    }

    // ----------------------------------------------------------------
    // Read-only accessor with the vendor plug-in's offset semantics; all bytes come from the cache.
    // ----------------------------------------------------------------
    internal sealed class RangeAccessor : IReadWriteAccessor, IReaderIssues
    {
        private readonly BlockCache _cache;

        private readonly bool _largeReads;

        public RangeAccessor(RemoteFile file, string streamId, long initialOffset, long size, bool permitMissingData)
        {
            _cache = file.Cache;
            _largeReads = file.LargeReads;
            StreamId = streamId;
            Length = file.Source.Length;
            InitialOffset = initialOffset;
            if (initialOffset > Length) throw new ArgumentException("offset beyond length of file", nameof(initialOffset));
            FileSizeExceeded = initialOffset + size > Length;
            if (FileSizeExceeded)
            {
                MaxSize = Length - initialOffset;
                if (!permitMissingData) throw new ArgumentException("size beyond end of file", nameof(size));
            }
            // The vendor batches record reads (scan data, trailers) into chunks of this size.
            // 0 makes it read records one by one through the block cache (fewer bytes on random
            // access, but much more CPU on full passes).
            SuggestedChunkSize = file.ChunkSize >= 0 ? file.ChunkSize
                : size > (1L << 30) ? 8 << 20 : size > (256L << 20) ? 4 << 20 : size > (64L << 20) ? 2 << 20 : 1 << 20;
        }

        public bool PreferLargeReads => _largeReads;
        public bool SupportsSubViews => true;
        public long InitialOffset { get; }
        public long Length { get; }
        public string StreamId { get; }
        public int SuggestedChunkSize { get; }
        public bool FileSizeExceeded { get; }
        public long MaxSize { get; }
        IReaderIssues IDisposableReader.ReaderIssues() => this;

        private BlockCache.Hot? _hot;

        private void Fill(long offset, Span<byte> dst)
        {
            long pos = InitialOffset + offset;
            var hot = _hot;
            if (hot != null && pos >= hot.Start && pos + dst.Length <= hot.Start + hot.Data.Length)
            {
                hot.Data.AsSpan((int)(pos - hot.Start), dst.Length).CopyTo(dst);
                return;
            }
            _cache.Read(pos, dst, out hot);
            if (hot != null) _hot = hot;
        }

        private T ReadValue<T>(long offset) where T : unmanaged
        {
            Span<byte> buf = stackalloc byte[Marshal.SizeOf<T>()];
            Fill(offset, buf);
            return MemoryMarshal.Read<T>(buf);
        }

        private T[] ReadArray<T>(long offset, int count) where T : unmanaged
        {
            var result = new T[count];
            if (count > 0) Fill(offset, MemoryMarshal.AsBytes(result.AsSpan()));
            return result;
        }

        public byte ReadByte(long offset) => ReadValue<byte>(offset);
        public short ReadShort(long offset) => ReadValue<short>(offset);
        public ushort ReadUnsignedShort(long offset) => ReadValue<ushort>(offset);
        public int ReadInt(long offset) => ReadValue<int>(offset);
        public uint ReadUnsignedInt(long offset) => ReadValue<uint>(offset);
        public float ReadFloat(long offset) => ReadValue<float>(offset);
        public double ReadDouble(long offset) => ReadValue<double>(offset);

        public byte[] ReadBytes(long offset, int count) => ReadArray<byte>(offset, count);
        public byte[] ReadLargeData(long startPos, int count) => ReadArray<byte>(startPos, count);
        public int[] ReadInts(long offset, int count) => ReadArray<int>(offset, count);
        public uint[] ReadUnsignedInts(long offset, int count) => ReadArray<uint>(offset, count);
        public float[] ReadFloats(long offset, int count) => ReadArray<float>(offset, count);
        public double[] ReadDoubles(long offset, int count) => ReadArray<double>(offset, count);

        public byte[] RentBytes(long offset, int count, IBufferPool pool)
        {
            var rented = pool.Rent(count);
            Fill(offset, rented.AsSpan(0, count));
            return rented;
        }

        public void ReturnRentedBytes(byte[] rented, IBufferPool pool) => pool.Release(rented);

        private static T BytesToStructure<T>(byte[] bytes) where T : struct
        {
            var handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            try { return Marshal.PtrToStructure<T>(handle.AddrOfPinnedObject()); }
            finally { handle.Free(); }
        }

        public T ReadStructure<T>(long offset, out long numberOfBytesRead) where T : struct
        {
            numberOfBytesRead = Marshal.SizeOf(typeof(T));
            return BytesToStructure<T>(ReadBytes(offset, (int)numberOfBytesRead));
        }

        public T ReadStructure<T>(long offset, int numberOfBytesToRead) where T : struct
            => BytesToStructure<T>(ReadBytes(offset, numberOfBytesToRead));

        public T ReadSimpleStructure<T>(long offset) where T : struct => ReadStructure<T>(offset, out _);

        public T ReadSimpleBlittableStructure<T>(long offset) where T : struct
            => MemoryMarshal.Read<T>(ReadBytes(offset, Marshal.SizeOf(typeof(T))));

        public T[] ReadSimpleStructureArray<T>(long offset, int count) where T : struct
        {
            var result = new T[count];
            for (int i = 0; i < count; i++)
            {
                result[i] = ReadStructure<T>(offset, out var n);
                offset += n;
            }
            return result;
        }

        public T[] ReadStructArray<T>(long offset, out long numberOfBytesRead) where T : struct
        {
            long pos = offset;
            int count = ReadInt(pos);
            pos += 4;
            var result = new T[count];
            for (int i = 0; i < count; i++)
            {
                result[i] = ReadStructure<T>(pos, out var n);
                pos += n;
            }
            numberOfBytesRead = pos - offset;
            return result;
        }

        public T ReadPreviousRevisionAndConvert<T>(long offset, int sizeOfPreviousRevStruct) where T : struct
        {
            var bytes = new byte[Marshal.SizeOf(typeof(T))];
            Fill(offset, bytes.AsSpan(0, sizeOfPreviousRevStruct));
            return BytesToStructure<T>(bytes);
        }

        private char[] ReadWideCharArray(long offset, ref long numOfBytesRead, uint stringSize)
        {
            if (stringSize == 0) return Array.Empty<char>();
            var chars = ReadArray<char>(offset, (int)stringSize);
            numOfBytesRead += stringSize * 2;
            return chars;
        }

        public string ReadString(long offset, out long numOfBytesRead)
        {
            uint size = ReadUnsignedInt(offset);
            numOfBytesRead = 4;
            return new string(ReadWideCharArray(offset + 4, ref numOfBytesRead, size));
        }

        public string ReadWideChars(long offset, out long numOfBytesRead)
        {
            uint size = ReadUnsignedInt(offset);
            numOfBytesRead = 4;
            return ReadWideChars(offset + 4, ref numOfBytesRead, size);
        }

        public string ReadWideChars(long offset, ref long numOfBytesRead, uint stringSize)
        {
            var chars = ReadWideCharArray(offset, ref numOfBytesRead, stringSize);
            int nul = Array.IndexOf(chars, '\0');
            return nul < 0 ? new string(chars) : new string(chars, 0, nul);
        }

        public string[] ReadStrings(long offset, out long numOfBytesRead)
        {
            long pos = offset;
            int count = ReadInt(pos);
            pos += 4;
            var result = new string[count];
            for (int i = 0; i < count; i++)
            {
                result[i] = ReadString(pos, out var n);
                pos += n;
            }
            numOfBytesRead = pos - offset;
            return result;
        }

        public IReadWriteAccessor? CreateSubView(long dataOffset, long blobSize)
            => blobSize < 2097152 ? new MemoryArrayAccessor(ReadLargeData(dataOffset, (int)blobSize), 0L) : null;

        public void Dispose() { }

        public long WriteStruct<T>(long offset, T data) where T : struct => throw new NotSupportedException();
        public long WriteStruct<T>(long offset, T data, int numBytesWrite) where T : struct => throw new NotSupportedException();
        public long WriteFloat(long offset, float value) => throw new NotSupportedException();
        public long WriteDouble(long offset, double value) => throw new NotSupportedException();
        public long WriteByte(long offset, byte value) => throw new NotSupportedException();
        public long WriteBytes(long offset, byte[] value) => throw new NotSupportedException();
        public long WriteShort(long offset, short value) => throw new NotSupportedException();
        public long WriteInt(long offset, int value) => throw new NotSupportedException();
        public int IncrementInt(long offset) => throw new NotSupportedException();
    }
}
