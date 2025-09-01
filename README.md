# libisyntax (Lambda-ready fork)

A library for decoding whole-slide images in Philips iSyntax format, adapted for AWS Lambda and heterogeneous Linux hosts.

## Differences from the original

This fork contains several fixes and changes for stability in serverless environments:

- **NULL cache safety**: `libisyntax/src/libisyntax.c` updates the public wrapper `libisyntax_tile_read()` to accept a `NULL` `isyntax_cache_t*`. If `NULL` is passed, a temporary cache (reusing existing allocators on the `isyntax` handle or creating temporary ones) is created, the tile read is performed, and temporary resources are cleaned up. This prevents segfaults when minimal callers omit a cache. Internal `isyntax_tile_read()` in `src/isyntax/isyntax_reader.c` still expects a non-null cache.
- **Thread/init guards**: Public entry points ensure global and thread-local initialization happens before file operations, avoiding uninitialized state in Lambda cold starts.
- **CMake for heterogeneous CPUs**: `libisyntax/CMakeLists.txt` avoids `-march=native` and disables STB SIMD with `STBI_NO_SIMD` to prevent illegal-instruction crashes on unknown host CPUs (e.g., Lambda). ARM/Apple Silicon flags are handled conditionally.
- **Benaphore file mutex**: Public functions lock `isyntax->file_mutex` around IO-sensitive operations to reduce race conditions in multi-threaded contexts.

## Building for Lambda (container image, preferred)

This repo can be built into a Lambda container image that already contains `libisyntax.so` under `/opt`. No S3 download is required at runtime.

Dockerfile used: project root `Dockerfile.lambda` (copies `libisyntax/`, builds `libisyntax.so`, places it in `/opt`, installs Python deps to `/opt/python`, and copies `lambda_function.py`).

Build locally:

```bash
docker build -f Dockerfile.lambda -t lambda-libisyntax:latest .
```

Test locally (Lambda Runtime Interface Emulator):

```bash
docker run -p 9000:8080 lambda-libisyntax:latest
# In another shell, invoke:
curl -s "http://localhost:9000/2015-03-31/functions/function/invocations" -d '{}'
```

Push to ECR and deploy as a Lambda (example):

```bash
AWS_ACCOUNT_ID=123456789012
AWS_REGION=us-east-1
REPO=lambda-libisyntax

# Create repo (once)
aws ecr create-repository --repository-name $REPO --region $AWS_REGION || true

# Authenticate Docker to ECR
aws ecr get-login-password --region $AWS_REGION | \
  docker login --username AWS --password-stdin ${AWS_ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com

# Tag and push
docker tag lambda-libisyntax:latest ${AWS_ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com/$REPO:latest
docker push ${AWS_ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com/$REPO:latest

# Create function (first time)
aws lambda create-function \
  --function-name libisyntax-test \
  --package-type Image \
  --code ImageUri=${AWS_ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com/$REPO:latest \
  --role arn:aws:iam::<account-id>:role/<lambda-execution-role> \
  --region $AWS_REGION

# Or update code on subsequent pushes
aws lambda update-function-code \
  --function-name libisyntax-test \
  --image-uri ${AWS_ACCOUNT_ID}.dkr.ecr.${AWS_REGION}.amazonaws.com/$REPO:latest \
  --region $AWS_REGION
```

Notes:
- Built on Amazon Linux base (`public.ecr.aws/lambda/python:3.11`) to match Lambda glibc.
- `CMakeLists.txt` avoids host-specific CPU flags and disables STB SIMD to prevent illegal instructions.

## Minimal Lambda usage (Python)

Example handler that loads `libisyntax.so` bundled inside the Lambda container image (no S3), opens a slide from `/opt` or `/var/task`, reads one tile, and returns basic info.

```python
import os, ctypes, tempfile

def load_api(path):
    lib = ctypes.CDLL(path)
    # minimal signatures used below
    lib.libisyntax_init.restype = ctypes.c_int
    lib.libisyntax_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
    lib.libisyntax_open.restype = ctypes.c_int
    lib.libisyntax_close.argtypes = [ctypes.c_void_p]
    lib.libisyntax_get_tile_width.argtypes = [ctypes.c_void_p]
    lib.libisyntax_get_tile_width.restype = ctypes.c_int
    lib.libisyntax_get_tile_height.argtypes = [ctypes.c_void_p]
    lib.libisyntax_get_tile_height.restype = ctypes.c_int
    lib.libisyntax_tile_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong, ctypes.c_longlong, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
    lib.libisyntax_tile_read.restype = ctypes.c_int
    return lib

def lambda_handler(event, context):
    # libisyntax.so is copied to /opt by Dockerfile.lambda
    lib = load_api("/opt/libisyntax.so")
    rc = lib.libisyntax_init()
    if rc != 0:
        raise RuntimeError(f"init failed: {rc}")

    # Provide a slide inside the image (e.g., copied to /opt/example.isyntax by Dockerfile.lambda)
    tmp_slide = "/opt/example.isyntax"

    handle = ctypes.c_void_p()
    rc = lib.libisyntax_open(tmp_slide.encode("utf-8"), 1, ctypes.byref(handle))
    if rc != 0:
        raise RuntimeError(f"open failed: {rc}")

    try:
        tw = lib.libisyntax_get_tile_width(handle)
        th = lib.libisyntax_get_tile_height(handle)
        buf = (ctypes.c_uint32 * (tw * th))()
        rc = lib.libisyntax_tile_read(handle, ctypes.c_void_p(0), 0, 0, 0, buf, 0x101)  # RGBA
        if rc != 0:
            raise RuntimeError(f"tile_read failed: {rc}")
        return {"tile_w": tw, "tile_h": th}
    finally:
        lib.libisyntax_close(handle)
```

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

## Optional: build-only artifact and S3 packaging

If you prefer a ZIP-based distribution of only `libisyntax.so`, `Dockerfile.build` can be used to compile on Amazon Linux, then you can zip and host it on S3. The Lambda handler can load from `/opt` or download to `/tmp`. This was the earlier research path and is not required when using container images.

## License

See `libisyntax/LICENSE.txt`.

