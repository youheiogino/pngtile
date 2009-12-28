#include "cache.h"
#include "shared/util.h"
#include "shared/log.h" // only LOG_DEBUG

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>


int pt_cache_new (struct pt_cache **cache_ptr, const char *path, int mode)
{
    struct pt_cache *cache;

    // alloc
    if ((cache = calloc(1, sizeof(*cache))) == NULL)
        return -1;

    if ((cache->path = strdup(path)) == NULL)
        goto error;

    // init
    cache->fd = -1;
    cache->mode = mode;

    // ok
    *cache_ptr = cache;

    return 0;

error:
    // cleanup
    if (cache)
        pt_cache_destroy(cache);

    return -1;
}

int pt_cache_status (struct pt_cache *cache, const char *img_path)
{
    struct stat st_img, st_cache;
    
    // test original file
    if (stat(img_path, &st_img) < 0)
        return -1;
    
    // test cache file
    if (stat(cache->path, &st_cache) < 0) {
        // always stale if it doesn't exist yet
        if (errno == ENOENT)
            return PT_CACHE_NONE;
        else
            return -1;
    }

    // compare mtime
    if (st_img.st_mtime > st_cache.st_mtime)
        return PT_CACHE_STALE;

    else
        return PT_CACHE_FRESH;
}

/**
 * Abort any incomplete open operation, cleaning up
 */
static void pt_cache_abort (struct pt_cache *cache)
{
    if (cache->header != NULL) {
        munmap(cache->header, PT_CACHE_HEADER_SIZE + cache->size);

        cache->header = NULL;
        cache->data = NULL;
    }

    if (cache->fd >= 0) {
        close(cache->fd);

        cache->fd = -1;
    }
}

/**
 * Open the cache file as an fd for reading
 *
 * XXX: needs locking
 */
static int pt_cache_open_read_fd (struct pt_cache *cache, int *fd_ptr)
{
    int fd;
    
    // actual open()
    if ((fd = open(cache->path, O_RDONLY)) < 0)
        return -1;

    // ok
    *fd_ptr = fd;

    return 0;
}

/**
 * Open the .tmp cache file as an fd for writing
 */
static int pt_cache_open_tmp_fd (struct pt_cache *cache, int *fd_ptr)
{
    int fd;
    char tmp_path[1024];
    
    // check mode
    if (!(cache->mode & PT_IMG_WRITE)) {
        errno = EPERM;
        return -1;
    }
    
    // get .tmp path
    if (path_with_fext(cache->path, tmp_path, sizeof(tmp_path), ".tmp"))
        return -1;

    // open for write, create
    // XXX: locking?
    if ((fd = open(tmp_path, O_RDWR | O_CREAT, 0644)) < 0)
        return -1;

    // ok
    *fd_ptr = fd;

    return 0;
}


/**
 * Mmap the opened cache file using PT_CACHE_HEADER_SIZE plus the calculated size stored in cache->size
 */
static int pt_cache_open_mmap (struct pt_cache *cache, void **addr_ptr, bool readonly)
{
    int prot = 0;
    void *addr;

    // determine prot
    prot |= PROT_READ;

    if (!readonly) {
        assert(cache->mode & PT_IMG_WRITE);

        prot |= PROT_WRITE;
    }

    // perform mmap() from second page on
    if ((addr = mmap(NULL, PT_CACHE_HEADER_SIZE + cache->size, prot, MAP_SHARED, cache->fd, 0)) == MAP_FAILED)
        return -1;

    // ok
    *addr_ptr = addr;

    return 0;
}

/**
 * Read in the cache header from the open file
 */
static int pt_cache_read_header (struct pt_cache *cache, struct pt_cache_header *header)
{
    size_t len = sizeof(*header);
    char *buf = (char *) header;

    // seek to start
    if (lseek(cache->fd, 0, SEEK_SET) != 0)
        return -1;

    // write out full header
    while (len) {
        ssize_t ret;
        
        // try and write out the header
        if ((ret = read(cache->fd, buf, len)) < 0)
            return -1;

        // update offset
        buf += ret;
        len -= ret;
    }

    // done
    return 0;
}

/**
 * Write out the cache header into the opened file
 */
static int pt_cache_write_header (struct pt_cache *cache, const struct pt_cache_header *header)
{
    size_t len = sizeof(*header);
    const char *buf = (const char *) header;

    // seek to start
    if (lseek(cache->fd, 0, SEEK_SET) != 0)
        return -1;

    // write out full header
    while (len) {
        ssize_t ret;
        
        // try and write out the header
        if ((ret = write(cache->fd, buf, len)) < 0)
            return -1;

        // update offset
        buf += ret;
        len -= ret;
    }

    // done
    return 0;
}

/**
 * Create a new .tmp cache file, open it, and write out the header.
 */
static int pt_cache_open_create (struct pt_cache *cache, struct pt_cache_header *header)
{
    void *base;

    // no access
    if (!(cache->mode & PT_IMG_WRITE)) {
        errno = EPERM;
        return -1;
    }

    // open as .tmp
    if (pt_cache_open_tmp_fd(cache, &cache->fd))
        return -1;

    // calculate data size
    cache->size = sizeof(*header) + header->height * header->row_bytes;

    // grow file
    if (ftruncate(cache->fd, PT_CACHE_HEADER_SIZE + cache->size) < 0)
        goto error;

    // mmap header and data
    if (pt_cache_open_mmap(cache, &base, false))
        goto error;

    cache->header = base;
    cache->data = base + PT_CACHE_HEADER_SIZE;

    // write header
    // XXX: should just mmap...
    if (pt_cache_write_header(cache, header))
        goto error;

    // done
    return 0;

error:
    // cleanup
    pt_cache_abort(cache);

    return -1;
}

/**
 * Rename the opened .tmp to .cache
 */
static int pt_cache_create_done (struct pt_cache *cache)
{
    char tmp_path[1024];
    
    // get .tmp path
    if (path_with_fext(cache->path, tmp_path, sizeof(tmp_path), ".tmp"))
        return -1;

    // rename
    if (rename(tmp_path, cache->path) < 0)
        return -1;

    // ok
    return 0;
}

int pt_cache_open (struct pt_cache *cache)
{
    struct pt_cache_header header;
    void *base;

    // open the .cache
    if (pt_cache_open_read_fd(cache, &cache->fd))
        return -1;

    // read in header
    if (pt_cache_read_header(cache, &header))
        return -1;

    // calculate data size
    cache->size = sizeof(header) + header.height * header.row_bytes;

    // mmap header and data
    if (pt_cache_open_mmap(cache, &base, true))
        goto error;

    cache->header = base;
    cache->data = base + PT_CACHE_HEADER_SIZE;

    // done
    return 0;

error:
    // cleanup
    pt_cache_abort(cache);

    return -1;
}

int pt_cache_update_png (struct pt_cache *cache, png_structp png, png_infop info)
{
    struct pt_cache_header header;
    
    // XXX: check cache_mode
    // XXX: check image doesn't use any options we don't handle
    // XXX: close any already-opened cache file

    memset(&header, 0, sizeof(header));

    // fill in basic info
    header.width = png_get_image_width(png, info);
    header.height = png_get_image_height(png, info);
    header.bit_depth = png_get_bit_depth(png, info);
    header.color_type = png_get_color_type(png, info);

    log_debug("width=%u, height=%u, bit_depth=%u, color_type=%u", 
            header.width, header.height, header.bit_depth, header.color_type
    );

    // only pack 1 pixel per byte
    if (header.bit_depth < 8)
        png_set_packing(png);

    // fill in other info
    header.row_bytes = png_get_rowbytes(png, info);

    // calculate bpp as num_channels * bpc
    // XXX: this assumes the packed bit depth will be either 8 or 16
    header.col_bytes = png_get_channels(png, info) * (header.bit_depth == 16 ? 2 : 1);

    log_debug("row_bytes=%u, col_bytes=%u", header.row_bytes, header.col_bytes);
    
    // palette etc.
    if (header.color_type == PNG_COLOR_TYPE_PALETTE) {
        int num_palette;
        png_colorp palette;

        if (png_get_PLTE(png, info, &palette, &num_palette) == 0)
            // XXX: PLTE chunk not read?
            return -1;
        
        // should only be 256 of them at most
        assert(num_palette <= PNG_MAX_PALETTE_LENGTH);
    
        // copy
        header.num_palette = num_palette;
        memcpy(&header.palette, palette, num_palette * sizeof(*palette));
        
        log_debug("num_palette=%u", num_palette);
    }

    // create .tmp and write out header
    if (pt_cache_open_create(cache, &header))
        return -1;


    // write out raw image data a row at a time
    for (size_t row = 0; row < header.height; row++) {
        // read row data, non-interlaced
        png_read_row(png, cache->data + row * header.row_bytes, NULL);
    }


    // move from .tmp to .cache
    if (pt_cache_create_done(cache))
        return -1;

    // done!
    return 0;
}

int pt_cache_tile_png (struct pt_cache *cache, png_structp png, png_infop info, const struct pt_tile_info *ti)
{
    if (!cache->data) {
        // not yet open
        if (pt_cache_open(cache))
            return -1;
    }

    // set basic info
    png_set_IHDR(png, info, ti->width, ti->height, cache->header->bit_depth, cache->header->color_type,
            PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
    );

    // set palette?
    if (cache->header->color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_PLTE(png, info, cache->header->palette, cache->header->num_palette);

    // write meta-info
    png_write_info(png, info);

    // pixel data is packed into 1 pixel per byte
    png_set_packing(png);
    
    // write image data
    for (size_t row = ti->y; row < ti->y + ti->height; row++) {
        size_t col = ti->x;
        
        // XXX: fill out-of-range regions in some background color
        png_write_row(png, cache->data + (row * cache->header->row_bytes) + (col * cache->header->col_bytes));
    }

    // done, flush remaining output
    png_write_flush(png);

    // ok
    return 0;
}

void pt_cache_destroy (struct pt_cache *cache)
{
    free(cache->path);

    pt_cache_abort(cache);

    free(cache);
}
