/*
  BSD 2-Clause License

  Copyright (c) 2019-2025, Pieter Valkema

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// To compile without implementing stb_printf.h:
// #define LIBISYNTAX_NO_STB_SPRINTF_IMPLEMENTATION

// To compile without implementing stb_image.h.:
// #define LIBISYNTAX_NO_STB_IMAGE_IMPLEMENTATION

// To compile without implementing thread pool routines (in case you want to supply your own):
// #define LIBISYNTAX_NO_THREAD_POOL_IMPLEMENTATION

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef LIBISYNTAX_NO_STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#endif
#include "stb_sprintf.h"

#ifndef LIBISYNTAX_NO_STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#endif
#include "stb_image.h"

#include "common.h"
#include "platform.h"
#include "intrinsics.h"

#include "libisyntax.h"
#include "isyntax.h"
#include "isyntax_reader.h"
#include <math.h>

#define CHECK_LIBISYNTAX_OK(_libisyntax_call) do { \
    isyntax_error_t result = _libisyntax_call;     \
    ASSERT(result == LIBISYNTAX_OK);               \
} while(0);


#ifndef LIBISYNTAX_NO_THREAD_POOL_IMPLEMENTATION

static platform_thread_info_t thread_infos[MAX_THREAD_COUNT];



// Routines for initializing the global thread pool

#if WINDOWS
#include "win32_utils.h"

_Noreturn DWORD WINAPI thread_proc(void* parameter) {
	platform_thread_info_t* thread_info = (platform_thread_info_t*) parameter;
	i64 init_start_time = get_clock();

	atomic_increment(&global_worker_thread_idle_count);

	init_thread_memory(thread_info->logical_thread_index, &global_system_info);
	thread_memory_t* thread_memory = local_thread_memory;

	for (i32 i = 0; i < MAX_ASYNC_IO_EVENTS; ++i) {
		thread_memory->async_io_events[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
		if (!thread_memory->async_io_events[i]) {
			win32_diagnostic("CreateEvent");
		}
	}
//	console_print("Thread %d reporting for duty (init took %.3f seconds)\n", thread_info->logical_thread_index, get_seconds_elapsed(init_start_time, get_clock()));

	for (;;) {
		if (thread_info->logical_thread_index > global_active_worker_thread_count) {
			// Worker is disabled, do nothing
			Sleep(100);
			continue;
		}
		if (!work_queue_is_work_in_progress(thread_info->queue)) {
			Sleep(1);
			WaitForSingleObjectEx(thread_info->queue->semaphore, 1, FALSE);
		}
        work_queue_do_work(thread_info->queue, thread_info->logical_thread_index);
	}
}

static void init_thread_pool() {
	init_thread_memory(0, &global_system_info);

    int total_thread_count = global_system_info.suggested_total_thread_count;
	global_worker_thread_count = total_thread_count - 1;
	global_active_worker_thread_count = global_worker_thread_count;

	global_work_queue = work_queue_create("/worksem", 1024); // Queue for newly submitted tasks
	global_completion_queue = work_queue_create("/completionsem", 1024); // Message queue for completed tasks

	// NOTE: the main thread is considered thread 0.
	for (i32 i = 1; i < total_thread_count; ++i) {
		platform_thread_info_t thread_info = { .logical_thread_index = i, .queue = &global_work_queue};
		thread_infos[i] = thread_info;

		DWORD thread_id;
		HANDLE thread_handle = CreateThread(NULL, 0, thread_proc, thread_infos + i, 0, &thread_id);
		CloseHandle(thread_handle);

	}


}

#else

#include <pthread.h>

static void* worker_thread(void* parameter) {
    platform_thread_info_t* thread_info = (platform_thread_info_t*) parameter;

//	fprintf(stderr, "Hello from thread %d\n", thread_info->logical_thread_index);

    init_thread_memory(thread_info->logical_thread_index, &global_system_info);
	atomic_increment(&global_worker_thread_idle_count);

	for (;;) {
		if (thread_info->logical_thread_index > global_active_worker_thread_count) {
			// Worker is disabled, do nothing
			platform_sleep(100);
			continue;
		}
        if (!work_queue_is_work_waiting_to_start(thread_info->queue)) {
            //platform_sleep(1);
            sem_wait(thread_info->queue->semaphore);
            if (thread_info->logical_thread_index > global_active_worker_thread_count) {
                // Worker is disabled, do nothing
                platform_sleep(100);
                continue;
            }
        }
        work_queue_do_work(thread_info->queue, thread_info->logical_thread_index);
    }

    return 0;
}

static void init_thread_pool() {
	init_thread_memory(0, &global_system_info);
    global_worker_thread_count = global_system_info.suggested_total_thread_count - 1;
    global_active_worker_thread_count = global_worker_thread_count;

	global_work_queue = work_queue_create("/worksem", 1024); // Queue for newly submitted tasks
	global_completion_queue = work_queue_create("/completionsem", 1024); // Message queue for completed tasks

    pthread_t threads[MAX_THREAD_COUNT] = {};

    // NOTE: the main thread is considered thread 0.
    for (i32 i = 1; i < global_system_info.suggested_total_thread_count; ++i) {
        thread_infos[i] = (platform_thread_info_t){ .logical_thread_index = i, .queue = &global_work_queue};

        if (pthread_create(threads + i, NULL, &worker_thread, (void*)(&thread_infos[i])) != 0) {
            fprintf(stderr, "Error creating thread\n");
        }

    }

    test_multithreading_work_queue();


}

#endif

#endif //LIBISYNTAX_NO_THREAD_POOL_IMPLEMENTATION

// TODO(avirodov): int may be too small for some counters later on.
// TODO(avirodov): should make a flag to turn counters off, they may have overhead.
// TODO(avirodov): struct? move to isyntax.h/.c?
// TODO(avirodov): debug api?
#define DBGCTR_COUNT(_counter) atomic_increment(&_counter)
i32 volatile dbgctr_init_thread_pool_counter = 0;
i32 volatile dbgctr_init_global_mutexes_created = 0;

static benaphore_t* libisyntax_get_global_mutex() {
    static benaphore_t libisyntax_global_mutex;
    static i32 volatile init_status = 0; // 0 - not initialized, 1 - being initialized, 2 - done initializing.

    // Quick path for already initialized scenario.
    read_barrier;
    if (init_status == 2) {
        return &libisyntax_global_mutex;
    }

    // We need to establish a global mutex, and this is nontrivial as mutex primitives available don't allow static
    // initialization (more discussion in https://github.com/amspath/libisyntax/issues/16).
    if (atomic_compare_exchange(&init_status, 1, 0)) {
        // We get to do the initialization
        libisyntax_global_mutex = benaphore_create();
        DBGCTR_COUNT(dbgctr_init_global_mutexes_created);
        init_status = 2;
        write_barrier;
    } else {
        // Wait until the other thread finishes initialization. Since we don't have a mutex, spinlock is
        // the best we can do here. It should be a very short critical section.
        do { read_barrier; } while(init_status < 2);
    }

    return &libisyntax_global_mutex;
}

/*
  Enhanced SEGFAULT FIX for libisyntax in Lambda environments
  
  Root cause analysis:
  1. local_thread_memory is NULL by default (thread-local variable)
  2. win32_overlapped_read() accesses local_thread_memory->temp_arena causing segfault
  3. This happens because thread memory isn't initialized in single-threaded Lambda environments
  
  Solution implemented:
  1. Use pthread_once for thread-safe global initialization
  2. Ensure thread memory is initialized before any file operations
  3. Add defensive checks for NULL pointers
*/

#include "libisyntax.h"
#include "isyntax.h"
#include "isyntax_reader.h"
#include "platform.h"
#include "block_allocator.h"
#include "benaphore.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

// Thread-safe global initialization
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static volatile bool g_is_initialized = false;

// Forward declarations for internal functions
static void ensure_global_initialization();
static void ensure_thread_memory();

// Internal function implementations
static void ensure_global_initialization() {
    if (!g_is_initialized) {
        // Initialize system info with proper error handling
        get_system_info(false);
        g_is_initialized = true;
    }
}

static void ensure_thread_memory() {
    // Critical fix: Always ensure thread memory is initialized
    if (!local_thread_memory) {
        init_thread_memory(0, &global_system_info);
    }
}

// Public API implementation with enhanced segfault fixes
isyntax_error_t libisyntax_init() {
    // Use pthread_once for guaranteed one-time initialization
    int result = pthread_once(&g_init_once, ensure_global_initialization);
    if (result != 0) {
        return LIBISYNTAX_FATAL;
    }
    
    // Ensure thread memory is initialized
    ensure_thread_memory();
    return LIBISYNTAX_OK;
}

isyntax_error_t libisyntax_open(const char* filename, enum libisyntax_open_flags_t flags, isyntax_t** out_isyntax) {
    if (!filename || !out_isyntax) {
        return LIBISYNTAX_INVALID_ARGUMENT;
    }
    
    // CRITICAL FIX: Initialize before ANY file operations
    int init_result = pthread_once(&g_init_once, ensure_global_initialization);
    if (init_result != 0) {
        return LIBISYNTAX_FATAL;
    }
    ensure_thread_memory();
    
    // Allocate and initialize isyntax structure with error checking
    isyntax_t* isyntax = (isyntax_t*)malloc(sizeof(isyntax_t));
    if (!isyntax) {
        return LIBISYNTAX_FATAL;
    }
    memset(isyntax, 0, sizeof(*isyntax));
    
    // Initialize the file mutex for thread safety
    isyntax->file_mutex = benaphore_create();
    
    // Call internal isyntax_open function with mutex protection
    benaphore_lock(&isyntax->file_mutex);
    bool success = isyntax_open(isyntax, filename, flags);
    benaphore_unlock(&isyntax->file_mutex);
    
    if (success) {
        *out_isyntax = isyntax;
        return LIBISYNTAX_OK;
    } else {
        free(isyntax);
        return LIBISYNTAX_FATAL;
    }
}

void libisyntax_close(isyntax_t* isyntax) {
    if (isyntax) {
        // Protect destruction with mutex
        benaphore_lock(&isyntax->file_mutex);
        isyntax_destroy(isyntax);
        benaphore_unlock(&isyntax->file_mutex);
        benaphore_destroy(&isyntax->file_mutex);
        free(isyntax);
    }
}

// Implement remaining getter functions
int32_t libisyntax_get_tile_width(const isyntax_t* isyntax) {
    return isyntax ? isyntax->tile_width : 0;
}

int32_t libisyntax_get_tile_height(const isyntax_t* isyntax) {
    return isyntax ? isyntax->tile_height : 0;
}

const isyntax_image_t* libisyntax_get_wsi_image(const isyntax_t* isyntax) {
    if (!isyntax || isyntax->wsi_image_index < 0) return NULL;
    return &isyntax->images[isyntax->wsi_image_index];
}

const isyntax_image_t* libisyntax_get_label_image(const isyntax_t* isyntax) {
    if (!isyntax || isyntax->label_image_index < 0) return NULL;
    return &isyntax->images[isyntax->label_image_index];
}

const isyntax_image_t* libisyntax_get_macro_image(const isyntax_t* isyntax) {
    if (!isyntax || isyntax->macro_image_index < 0) return NULL;
    return &isyntax->images[isyntax->macro_image_index];
}

const char* libisyntax_get_barcode(const isyntax_t* isyntax) {
    return isyntax ? isyntax->barcode : NULL;
}

int32_t libisyntax_image_get_level_count(const isyntax_image_t* image) {
    return image ? image->level_count : 0;
}

int32_t libisyntax_image_get_offset_x(const isyntax_image_t* image) {
    return image ? image->offset_x : 0;
}

int32_t libisyntax_image_get_offset_y(const isyntax_image_t* image) {
    return image ? image->offset_y : 0;
}

const isyntax_level_t* libisyntax_image_get_level(const isyntax_image_t* image, int32_t index) {
    if (!image || index < 0 || index >= image->level_count) return NULL;
    return &image->levels[index];
}

int32_t libisyntax_level_get_scale(const isyntax_level_t* level) {
    return level ? level->scale : 0;
}

int32_t libisyntax_level_get_width_in_tiles(const isyntax_level_t* level) {
    return level ? level->width_in_tiles : 0;
}

int32_t libisyntax_level_get_height_in_tiles(const isyntax_level_t* level) {
    return level ? level->height_in_tiles : 0;
}

int32_t libisyntax_level_get_width(const isyntax_level_t* level) {
    return level ? level->width : 0;
}

int32_t libisyntax_level_get_height(const isyntax_level_t* level) {
    return level ? level->height : 0;
}

float libisyntax_level_get_mpp_x(const isyntax_level_t* level) {
    return level ? level->um_per_pixel_x : 0.0f;
}

float libisyntax_level_get_mpp_y(const isyntax_level_t* level) {
    return level ? level->um_per_pixel_y : 0.0f;
}

// Cache management functions
isyntax_error_t libisyntax_cache_create(const char* debug_name_or_null, int32_t cache_size,
                                        isyntax_cache_t** out_isyntax_cache) {
    isyntax_cache_t* cache_ptr = (isyntax_cache_t*)malloc(sizeof(isyntax_cache_t));
    if (!cache_ptr) return LIBISYNTAX_FATAL;
    
    memset(cache_ptr, 0, sizeof(*cache_ptr));
    tile_list_init(&cache_ptr->cache_list, debug_name_or_null);
    cache_ptr->target_cache_size = cache_size;
    cache_ptr->mutex = benaphore_create();
    
    *out_isyntax_cache = cache_ptr;
    return LIBISYNTAX_OK;
}

isyntax_error_t libisyntax_cache_inject(isyntax_cache_t* isyntax_cache, isyntax_t* isyntax) {
    if (!isyntax_cache || !isyntax) return LIBISYNTAX_INVALID_ARGUMENT;
    if (isyntax->ll_coeff_block_allocator != NULL || isyntax->h_coeff_block_allocator != NULL) {
        return LIBISYNTAX_INVALID_ARGUMENT;
    }

    isyntax_cache->allocator_block_width = isyntax->block_width;
    isyntax_cache->allocator_block_height = isyntax->block_height;
    size_t ll_coeff_block_size = isyntax->block_width * isyntax->block_height * sizeof(icoeff_t);
    size_t block_allocator_maximum_capacity_in_blocks = GIGABYTES(32) / ll_coeff_block_size;
    size_t ll_coeff_block_allocator_capacity_in_blocks = block_allocator_maximum_capacity_in_blocks / 4;
    size_t h_coeff_block_size = ll_coeff_block_size * 3;
    size_t h_coeff_block_allocator_capacity_in_blocks = ll_coeff_block_allocator_capacity_in_blocks * 3;
    
    isyntax_cache->ll_coeff_block_allocator = (block_allocator_t*)malloc(sizeof(block_allocator_t));
    isyntax_cache->h_coeff_block_allocator = (block_allocator_t*)malloc(sizeof(block_allocator_t));
    
    if (!isyntax_cache->ll_coeff_block_allocator || !isyntax_cache->h_coeff_block_allocator) {
        return LIBISYNTAX_FATAL;
    }
    
    *isyntax_cache->ll_coeff_block_allocator = block_allocator_create(ll_coeff_block_size, ll_coeff_block_allocator_capacity_in_blocks, MEGABYTES(256));
    *isyntax_cache->h_coeff_block_allocator = block_allocator_create(h_coeff_block_size, h_coeff_block_allocator_capacity_in_blocks, MEGABYTES(256));
    isyntax_cache->is_block_allocator_owned = true;

    if (isyntax_cache->allocator_block_width != isyntax->block_width ||
            isyntax_cache->allocator_block_height != isyntax->block_height) {
        return LIBISYNTAX_FATAL;
    }

    isyntax->ll_coeff_block_allocator = isyntax_cache->ll_coeff_block_allocator;
    isyntax->h_coeff_block_allocator = isyntax_cache->h_coeff_block_allocator;
    isyntax->is_block_allocator_owned = false;
    return LIBISYNTAX_OK;
}

void libisyntax_cache_destroy(isyntax_cache_t* isyntax_cache) {
    if (!isyntax_cache) return;
    
    if (isyntax_cache->is_block_allocator_owned) {
        if (isyntax_cache->ll_coeff_block_allocator && isyntax_cache->ll_coeff_block_allocator->is_valid) {
            block_allocator_destroy(isyntax_cache->ll_coeff_block_allocator);
        }
        if (isyntax_cache->h_coeff_block_allocator && isyntax_cache->h_coeff_block_allocator->is_valid) {
            block_allocator_destroy(isyntax_cache->h_coeff_block_allocator);
        }
    }

    benaphore_destroy(&isyntax_cache->mutex);
    free(isyntax_cache->ll_coeff_block_allocator);
    free(isyntax_cache->h_coeff_block_allocator);
    free(isyntax_cache);
}

// Tile reading function
isyntax_error_t libisyntax_tile_read(isyntax_t* isyntax, isyntax_cache_t* isyntax_cache,
                                     int32_t level, int64_t tile_x, int64_t tile_y,
                                     uint32_t* pixels_buffer, int32_t pixel_format) {
    if (!isyntax || !pixels_buffer) return LIBISYNTAX_INVALID_ARGUMENT;
    if (pixel_format <= _LIBISYNTAX_PIXEL_FORMAT_START || pixel_format >= _LIBISYNTAX_PIXEL_FORMAT_END) {
        return LIBISYNTAX_INVALID_ARGUMENT;
    }

    // If no cache is provided, construct a temporary one.
    bool used_temp_cache = false;
    bool temp_allocators_owned = false;
    isyntax_cache_t temp_cache = {0};

    if (isyntax_cache == NULL) {
        used_temp_cache = true;
        // Initialize cache bookkeeping
        tile_list_init(&temp_cache.cache_list, "temp_cache");
        temp_cache.target_cache_size = 1024; // reasonable default for one-shot reads
        temp_cache.mutex = benaphore_create();

        // Prefer using allocators that may already be initialized on isyntax
        if (isyntax->ll_coeff_block_allocator && isyntax->h_coeff_block_allocator) {
            temp_cache.ll_coeff_block_allocator = isyntax->ll_coeff_block_allocator;
            temp_cache.h_coeff_block_allocator = isyntax->h_coeff_block_allocator;
            temp_cache.is_block_allocator_owned = false;
        } else {
            // Create temporary allocators compatible with this isyntax
            temp_allocators_owned = true;
            temp_cache.is_block_allocator_owned = true;
            temp_cache.allocator_block_width = isyntax->block_width;
            temp_cache.allocator_block_height = isyntax->block_height;
            size_t ll_coeff_block_size = (size_t)isyntax->block_width * (size_t)isyntax->block_height * sizeof(icoeff_t);
            size_t block_allocator_maximum_capacity_in_blocks = GIGABYTES(32) / ll_coeff_block_size;
            size_t ll_coeff_block_allocator_capacity_in_blocks = block_allocator_maximum_capacity_in_blocks / 4;
            size_t h_coeff_block_size = ll_coeff_block_size * 3;
            size_t h_coeff_block_allocator_capacity_in_blocks = ll_coeff_block_allocator_capacity_in_blocks * 3;

            temp_cache.ll_coeff_block_allocator = (block_allocator_t*)malloc(sizeof(block_allocator_t));
            temp_cache.h_coeff_block_allocator = (block_allocator_t*)malloc(sizeof(block_allocator_t));
            if (!temp_cache.ll_coeff_block_allocator || !temp_cache.h_coeff_block_allocator) {
                // Cleanup partially allocated resources
                if (temp_cache.ll_coeff_block_allocator) free(temp_cache.ll_coeff_block_allocator);
                if (temp_cache.h_coeff_block_allocator) free(temp_cache.h_coeff_block_allocator);
                benaphore_destroy(&temp_cache.mutex);
                return LIBISYNTAX_FATAL;
            }

            *temp_cache.ll_coeff_block_allocator = block_allocator_create(ll_coeff_block_size,
                                                                           ll_coeff_block_allocator_capacity_in_blocks,
                                                                           MEGABYTES(256));
            *temp_cache.h_coeff_block_allocator  = block_allocator_create(h_coeff_block_size,
                                                                           h_coeff_block_allocator_capacity_in_blocks,
                                                                           MEGABYTES(256));
        }

        isyntax_cache = &temp_cache;
    }

    // Protect tile reading with the file mutex to serialize filesystem IO
    benaphore_lock(&isyntax->file_mutex);
    isyntax_tile_read(isyntax, isyntax_cache, level, tile_x, tile_y, pixels_buffer, pixel_format);
    benaphore_unlock(&isyntax->file_mutex);

    // Cleanup temporary cache if created
    if (used_temp_cache) {
        if (temp_allocators_owned && temp_cache.ll_coeff_block_allocator && temp_cache.ll_coeff_block_allocator->is_valid) {
            block_allocator_destroy(temp_cache.ll_coeff_block_allocator);
        }
        if (temp_allocators_owned && temp_cache.h_coeff_block_allocator && temp_cache.h_coeff_block_allocator->is_valid) {
            block_allocator_destroy(temp_cache.h_coeff_block_allocator);
        }
        if (temp_allocators_owned) {
            free(temp_cache.ll_coeff_block_allocator);
            free(temp_cache.h_coeff_block_allocator);
        }
        benaphore_destroy(&temp_cache.mutex);
    }

    return LIBISYNTAX_OK;
}

// Associated image reading functions
isyntax_error_t libisyntax_read_macro_image_jpeg(isyntax_t* isyntax, uint8_t** jpeg_buffer, uint32_t* jpeg_size) {
    if (!isyntax || !jpeg_buffer || !jpeg_size) return LIBISYNTAX_INVALID_ARGUMENT;

    benaphore_lock(&isyntax->file_mutex);
    isyntax_image_t* macro_image = &isyntax->images[isyntax->macro_image_index];
    u8* jpeg_compressed = isyntax_get_associated_image_jpeg(isyntax, macro_image, jpeg_size);
    benaphore_unlock(&isyntax->file_mutex);
    
    if (jpeg_compressed) {
        *jpeg_buffer = jpeg_compressed;
        return LIBISYNTAX_OK;
    } else {
        return LIBISYNTAX_FATAL;
    }
}

isyntax_error_t libisyntax_read_label_image_jpeg(isyntax_t* isyntax, uint8_t** jpeg_buffer, uint32_t* jpeg_size) {
    if (!isyntax || !jpeg_buffer || !jpeg_size) return LIBISYNTAX_INVALID_ARGUMENT;
    
    benaphore_lock(&isyntax->file_mutex);
    isyntax_image_t* label_image = &isyntax->images[isyntax->label_image_index];
    u8* jpeg_compressed = isyntax_get_associated_image_jpeg(isyntax, label_image, jpeg_size);
    benaphore_unlock(&isyntax->file_mutex);
    
    if (jpeg_compressed) {
        *jpeg_buffer = jpeg_compressed;
        return LIBISYNTAX_OK;
    } else {
        return LIBISYNTAX_FATAL;
    }
}

isyntax_error_t libisyntax_read_region(isyntax_t* isyntax, isyntax_cache_t* isyntax_cache, int32_t level, int64_t x, int64_t y, int64_t width, int64_t height, uint32_t* pixels_buffer, int32_t pixel_format) {
    if (!isyntax || !pixels_buffer || width <= 0 || height <= 0) {
        return LIBISYNTAX_INVALID_ARGUMENT;
    }
    if (pixel_format <= _LIBISYNTAX_PIXEL_FORMAT_START || pixel_format >= _LIBISYNTAX_PIXEL_FORMAT_END) {
        return LIBISYNTAX_INVALID_ARGUMENT;
    }

    const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax);
    if (!wsi) return LIBISYNTAX_INVALID_ARGUMENT;

    const isyntax_level_t* level_info = libisyntax_image_get_level(wsi, level);
    if (!level_info) return LIBISYNTAX_INVALID_ARGUMENT;

    int32_t tile_width = libisyntax_get_tile_width(isyntax);
    int32_t tile_height = libisyntax_get_tile_height(isyntax);

    // Allocate a temporary buffer for reading tiles.
    uint32_t* tile_buffer = (uint32_t*)malloc((size_t)tile_width * tile_height * sizeof(uint32_t));
    if (!tile_buffer) {
        return LIBISYNTAX_FATAL;
    }

    // Calculate tile boundaries for the requested region
    int32_t start_tile_x = (int32_t)(x / tile_width);
    int32_t start_tile_y = (int32_t)(y / tile_height);
    int32_t end_tile_x = (int32_t)((x + width - 1) / tile_width);
    int32_t end_tile_y = (int32_t)((y + height - 1) / tile_height);

    for (int32_t ty = start_tile_y; ty <= end_tile_y; ++ty) {
        for (int32_t tx = start_tile_x; tx <= end_tile_x; ++tx) {
            // Read the entire tile that contains a portion of our region.
            isyntax_error_t read_result = libisyntax_tile_read(isyntax, isyntax_cache, level, tx, ty, tile_buffer, pixel_format);
            if (read_result != LIBISYNTAX_OK) {
                free(tile_buffer);
                return read_result; // Propagate the error
            }

            // Calculate the intersection of the requested region and the current tile.
            int64_t tile_origin_x = (int64_t)tx * tile_width;
            int64_t tile_origin_y = (int64_t)ty * tile_height;

            int64_t intersect_x0 = (x > tile_origin_x) ? x : tile_origin_x;
            int64_t intersect_y0 = (y > tile_origin_y) ? y : tile_origin_y;
            int64_t intersect_x1 = (x + width < tile_origin_x + tile_width) ? (x + width) : (tile_origin_x + tile_width);
            int64_t intersect_y1 = (y + height < tile_origin_y + tile_height) ? (y + height) : (tile_origin_y + tile_height);

            // Copy the intersecting pixels from the tile buffer to the output buffer.
            for (int64_t src_y = intersect_y0; src_y < intersect_y1; ++src_y) {
                int64_t dest_y = src_y - y;
                for (int64_t src_x = intersect_x0; src_x < intersect_x1; ++src_x) {
                    int64_t dest_x = src_x - x;
                    int64_t src_index = (src_y - tile_origin_y) * tile_width + (src_x - tile_origin_x);
                    int64_t dest_index = dest_y * width + dest_x;
                    pixels_buffer[dest_index] = tile_buffer[src_index];
                }
            }
        }
    }

    free(tile_buffer);
    return LIBISYNTAX_OK;
}