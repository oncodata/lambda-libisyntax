# libisyntax (Lambda-ready fork)

A library for decoding whole-slide images in Philips iSyntax format, adapted for AWS Lambda and heterogeneous Linux hosts.

## Differences from the original

This fork contains several fixes and changes for stability in serverless environments:

- **NULL cache safety**: `libisyntax/src/libisyntax.c` updates the public wrapper `libisyntax_tile_read()` to accept a `NULL` `isyntax_cache_t*`. If `NULL` is passed, a temporary cache (reusing existing allocators on the `isyntax` handle or creating temporary ones) is created, the tile read is performed, and temporary resources are cleaned up. This prevents segfaults when minimal callers omit a cache. Internal `isyntax_tile_read()` in `src/isyntax/isyntax_reader.c` still expects a non-null cache.
- **Thread/init guards**: Public entry points ensure global and thread-local initialization happens before file operations, avoiding uninitialized state in Lambda cold starts.
- **CMake for heterogeneous CPUs**: `libisyntax/CMakeLists.txt` avoids `-march=native` and disables STB SIMD with `STBI_NO_SIMD` to prevent illegal-instruction crashes on unknown host CPUs (e.g., Lambda). ARM/Apple Silicon flags are handled conditionally.
- **Benaphore file mutex**: Public functions lock `isyntax->file_mutex` around IO-sensitive operations to reduce race conditions in multi-threaded contexts.

## Building and packaging for Lambda

The typical deployment flow in this repository packages the shared library separately and downloads it at runtime in Lambda:

1. Build the shared object `libisyntax.so` inside a Lambda-compatible container (see project root `Dockerfile.build`).
2. Zip the artifact and upload to S3 (see project root `build_and_upload.ps1`).
3. The Lambda function (`lambda_function.py` in the project root) downloads `libisyntax.zip` from S3 into `/tmp`, extracts it, and `ctypes.CDLL` loads `libisyntax.so`.

Notes:
- The library must be built on/for Amazon Linux compatible glibc to load in Python 3.11 Lambda.
- Keep CPU features generic (no host-specific `-march`) to avoid `SIGILL` on Lambda hosts.

## Using the library safely (C API)

Minimum happy path:

```c
// 1) Initialize once per process
libisyntax_init();

// 2) Open a file (flag 1 initializes internal allocators on the handle)
isyntax_t* handle = NULL;
libisyntax_open("/path/slide.isyntax", 1 /* LIBISYNTAX_OPEN_FLAG_INIT_ALLOCATORS */, &handle);

// 3a) One-shot tile read (no explicit cache) – safe in this fork
uint32_t* pixels = malloc(256 * 256 * sizeof(uint32_t));
libisyntax_tile_read(handle, NULL, 0, 0, 0, pixels, 0x101 /* RGBA */);

// 3b) Preferred for performance: create and reuse a cache
// libisyntax_cache_t* cache; libisyntax_cache_create("app-cache", 1024, &cache);
// libisyntax_cache_inject(cache, handle);
// libisyntax_tile_read(handle, cache, level, tile_x, tile_y, pixels, 0x101);
// libisyntax_cache_destroy(cache);

// 4) Close
libisyntax_close(handle);
free(pixels);
```

Recommendations:
- Reuse a cache for multiple tile reads to avoid repeated allocator setup/teardown.
- The temporary cache path is provided for safety and convenience but is slower for many reads.

## Python (ctypes) usage in Lambda

See `lambda_function.py` in the project root for a complete example. Key bindings used:

- Initialization/open/close:
  - `libisyntax_init()`
  - `libisyntax_open(const char* filename, int flags, isyntax_t** out)`
  - `libisyntax_close(isyntax_t* ptr)`
- Metadata:
  - `libisyntax_get_tile_width/height(isyntax_t*)`
  - `libisyntax_get_wsi_image(isyntax_t*) -> isyntax_image_t*`
  - `libisyntax_image_get_level_count/offset_x/offset_y(isyntax_image_t*)`
  - `libisyntax_image_get_level(isyntax_image_t*, int index) -> isyntax_level_t*`
  - Level getters: width/height, tiles, scale, mpp_x/mpp_y
  - Associated images: `libisyntax_get_macro_image()`, `libisyntax_get_label_image()` and `libisyntax_read_macro_image_jpeg()` / `libisyntax_read_label_image_jpeg()`
- Tile read:
  - `libisyntax_tile_read(isyntax_t*, isyntax_cache_t* /*nullable in this fork*/, level, x, y, uint32_t* rgba, pixel_format)`

The Lambda example exposes:
- `metadata.levels`: an array with all WSI levels’ dimensions/MPP.
- `associated_images`: macro/label `jpeg_size` and a short hex preview.
- A sample tile read from level 0 at (0,0) into an RGBA buffer.

### Why argtypes/restype matter with ctypes

By default, `ctypes` assumes `int` for all parameters and return types, which is unsafe for pointers and 64-bit values and can cause crashes. Always specify accurate signatures to ensure correct marshalling:

```python
lib = ctypes.CDLL(lib_path)

# Initialization/open/close
lib.libisyntax_init.restype = ctypes.c_int
lib.libisyntax_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
lib.libisyntax_open.restype = ctypes.c_int
lib.libisyntax_close.argtypes = [ctypes.c_void_p]

# Metadata getters
lib.libisyntax_get_tile_width.argtypes = [ctypes.c_void_p]
lib.libisyntax_get_tile_width.restype = ctypes.c_int
lib.libisyntax_get_barcode.argtypes = [ctypes.c_void_p]
lib.libisyntax_get_barcode.restype = ctypes.c_char_p

# Object graph
lib.libisyntax_get_wsi_image.argtypes = [ctypes.c_void_p]
lib.libisyntax_get_wsi_image.restype = ctypes.c_void_p
lib.libisyntax_image_get_level.argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libisyntax_image_get_level.restype = ctypes.c_void_p
lib.libisyntax_level_get_mpp_x.restype = ctypes.c_float

# Associated JPEGs
lib.libisyntax_read_macro_image_jpeg.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
    ctypes.POINTER(ctypes.c_uint32),
]
lib.libisyntax_read_macro_image_jpeg.restype = ctypes.c_int

# Tile read
lib.libisyntax_tile_read.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p,
    ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong,
    ctypes.POINTER(ctypes.c_uint32), ctypes.c_int,
]
lib.libisyntax_tile_read.restype = ctypes.c_int
```

## CMake notes

Relevant bits from `libisyntax/CMakeLists.txt`:

- Avoid CPU-specific flags that could break on Lambda.
- Adds `STBI_NO_SIMD` to bypass STB SIMD code paths.
- Conditionally configures Apple Silicon flags.

Build example (inside a compatible Docker image or Amazon Linux):

```bash
cmake -S libisyntax -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# Resulting shared library location depends on your toolchain setup
```

## License

See `libisyntax/LICENSE.txt`.

