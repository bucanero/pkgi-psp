#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpng16/png.h>
#include <SDL2/SDL.h>

#include "pkgi.h"
//#include "menu.h"

#define PNG_SIGSIZE (8)

typedef struct rawImage
{
	uint32_t *datap;
	unsigned short width;
	unsigned short height;
} rawImage_t __attribute__ ((aligned (16)));


extern SDL_Renderer* renderer;

static rawImage_t * imgCreateEmptyTexture(unsigned int w, unsigned int h)
{
	rawImage_t *img=NULL;
	img=malloc(sizeof(rawImage_t));
	if(img!=NULL)
	{
		img->datap=malloc(w*h*4);
		if(img->datap==NULL)
		{
			free(img);
			return NULL;
		}
		img->width=w;
		img->height=h;
	}
	return img;
}

static void imgReadPngFromBuffer(png_structp png_ptr, png_bytep data, png_size_t length)
{
	png_voidp *address = png_get_io_ptr(png_ptr);
	memcpy(data, (void *)*address, length);
	*address += length;
}

static rawImage_t *imgLoadPngGeneric(const void *io_ptr, png_rw_ptr read_data_fn)
{
	png_structp png_ptr=png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
	if (png_ptr==NULL)
		goto error_create_read;

	png_infop info_ptr=png_create_info_struct(png_ptr);
	if (info_ptr==NULL)
		goto error_create_info;

	png_bytep *row_ptrs=NULL;

	if (setjmp(png_jmpbuf(png_ptr))) 
	{
		png_destroy_read_struct(&png_ptr,&info_ptr,(png_infopp)0);
		if (row_ptrs!=NULL)
			free(row_ptrs);

		return NULL;
	}

	png_set_read_fn(png_ptr,(png_voidp)io_ptr,read_data_fn);
	png_set_sig_bytes(png_ptr,PNG_SIGSIZE);
	png_read_info(png_ptr,info_ptr);

	unsigned int width, height;
	int bit_depth, color_type;

	png_get_IHDR(png_ptr,info_ptr,&width,&height,&bit_depth,&color_type,NULL,NULL,NULL);

	if ((color_type==PNG_COLOR_TYPE_PALETTE && bit_depth<=8)
		|| (color_type==PNG_COLOR_TYPE_GRAY && bit_depth<8)
		|| png_get_valid(png_ptr,info_ptr,PNG_INFO_tRNS)
		|| (bit_depth==16))
	{
		png_set_expand(png_ptr);
	}

	if (bit_depth == 16)
		png_set_scale_16(png_ptr);

	if (bit_depth==8 && color_type==PNG_COLOR_TYPE_RGB)
		png_set_filler(png_ptr,0xFF,PNG_FILLER_AFTER);

	if (color_type==PNG_COLOR_TYPE_GRAY ||
	    color_type==PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png_ptr);

	if (color_type==PNG_COLOR_TYPE_PALETTE) {
		png_set_palette_to_rgb(png_ptr);
		png_set_filler(png_ptr,0xFF,PNG_FILLER_AFTER);
	}

	if (color_type==PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png_ptr);

	if (png_get_valid(png_ptr,info_ptr,PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png_ptr);

	if (bit_depth<8)
		png_set_packing(png_ptr);

	png_read_update_info(png_ptr, info_ptr);

	row_ptrs = (png_bytep *)malloc(sizeof(png_bytep)*height);
	if (!row_ptrs)
		goto error_alloc_rows;

	rawImage_t *texture = imgCreateEmptyTexture(width,height);
	if (!texture)
		goto error_create_tex;

	for (int i=0; i<height; i++)
		row_ptrs[i]=(png_bytep)(texture->datap + i*width);

	png_read_image(png_ptr, row_ptrs);

	png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp)0);
	free(row_ptrs);

	return texture;

error_create_tex:
	free(row_ptrs);
error_alloc_rows:
	png_destroy_info_struct(png_ptr,&info_ptr);
error_create_info:
	png_destroy_read_struct(&png_ptr,(png_infopp)0,(png_infopp)0);
error_create_read:
	return NULL;
}

static rawImage_t *imgLoadPngFromBuffer(const void *buffer)
{
	if(png_sig_cmp((png_byte *)buffer, 0, PNG_SIGSIZE) != 0) 
		return NULL;

	uint64_t buffer_address=(uint32_t)buffer+PNG_SIGSIZE;

	return imgLoadPngGeneric((void *)&buffer_address, imgReadPngFromBuffer);
}

static rawImage_t *imgLoadPngFromFile(const char *path)
{
    uint8_t *buf;
    size_t size;
    rawImage_t *img;

    size = pkgi_get_size(path);
    if (size < 0)
        return NULL;

    buf = malloc(size);
    if (!buf)
        return NULL;

    if(pkgi_load(path, buf, size) < 0)
        return NULL;

    img = imgLoadPngFromBuffer(buf);
    free(buf);

    return img;
}

pkgi_texture loadPngTexture(const char* path, const void* buffer)
{
	rawImage_t* raw;
	pkgi_texture tex;

	tex = malloc(sizeof(struct pkgi_texture_s));
	if (!tex)
		return NULL;

	raw = path ? imgLoadPngFromFile(path) : imgLoadPngFromBuffer(buffer);
	if (!raw)
	{
		free(tex);
		return NULL;
	}

	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(raw->datap, raw->width, raw->height, 32, 4 * raw->width,
												0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);

	tex->texture = SDL_CreateTextureFromSurface(renderer, surface);
	tex->width = raw->width;
	tex->height = raw->height;

	SDL_FreeSurface(surface);
	free(raw->datap);
	free(raw);

	return tex;
}
