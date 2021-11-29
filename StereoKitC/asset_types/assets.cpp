#include "assets.h"
#include "../_stereokit.h"
#include "../sk_memory.h"

#include "mesh.h"
#include "texture.h"
#include "shader.h"
#include "material.h"
#include "model.h"
#include "font.h"
#include "sprite.h"
#include "sound.h"
#include "../systems/physics.h"
#include "../libraries/stref.h"
#include "../libraries/ferr_hash.h"
#include "../libraries/array.h"
#include "../libraries/tinycthread.h"

#include <stdio.h>
#include <assert.h>

namespace sk {

///////////////////////////////////////////

typedef struct asset_thread_t {
	asset_job_category_  category;
	array_t<asset_job_t> jobs;
	mtx_t                job_mtx;
	thrd_t               thread;
	bool32_t             run;
	bool32_t             running;
} asset_thread_t;

///////////////////////////////////////////

array_t<asset_header_t *> assets = {};
array_t<asset_header_t *> assets_multithread_destroy = {};
mtx_t                     assets_multithread_destroy_lock;
asset_thread_t            asset_threads[3] = {};
const asset_job_category_ asset_blocking_thread = asset_job_category_gpu;

///////////////////////////////////////////

int32_t assets_thread(void *arg);

///////////////////////////////////////////

void *assets_find(const char *id, asset_type_ type) {
	return assets_find(hash_fnv64_string(id), type);
}

///////////////////////////////////////////

void *assets_find(uint64_t id, asset_type_ type) {
	size_t count = assets.count;
	for (size_t i = 0; i < count; i++) {
		if (assets[i]->id == id && assets[i]->type == type)
			return assets[i];
	}
	return nullptr;
}

///////////////////////////////////////////

void assets_unique_name(asset_type_ type, const char *root_name, char *dest, int dest_size) {
	snprintf(dest, dest_size, "%s", root_name);
	uint64_t id    = hash_fnv64_string(dest);
	int      count = 1;
	while (assets_find(dest, type) != nullptr) {
		snprintf(dest, dest_size, "%s%d", root_name, count);
		id = hash_fnv64_string(dest);
		count += 1;
	}
}

///////////////////////////////////////////

void *assets_allocate(asset_type_ type) {
	size_t size = sizeof(asset_header_t);
	switch(type) {
	case asset_type_mesh:     size = sizeof(_mesh_t );    break;
	case asset_type_texture:  size = sizeof(_tex_t);      break;
	case asset_type_shader:   size = sizeof(_shader_t);   break;
	case asset_type_material: size = sizeof(_material_t); break;
	case asset_type_model:    size = sizeof(_model_t);    break;
	case asset_type_font:     size = sizeof(_font_t);     break;
	case asset_type_sprite:   size = sizeof(_sprite_t);   break;
	case asset_type_sound:    size = sizeof(_sound_t);    break;
	case asset_type_solid:    size = sizeof(_solid_t);    break;
	default: log_err("Unimplemented asset type!"); abort();
	}

	char name[64];
	snprintf(name, sizeof(name), "auto/asset_%zu", assets.count);

	asset_header_t *header = (asset_header_t *)sk_malloc(size);
	memset(header, 0, size);
	header->type  = type;
	header->refs += 1;
	header->id    = hash_fnv64_string(name);
	header->index = assets.count;
	assets.add(header);
	return header;
}

///////////////////////////////////////////

void assets_set_id(asset_header_t &header, const char *id) {
	assets_set_id(header, hash_fnv64_string(id));
#if defined(SK_DEBUG)
	header.id_text = string_copy(id);
#endif
}

///////////////////////////////////////////

void assets_set_id(asset_header_t &header, uint64_t id) {
#if defined(SK_DEBUG)
	asset_header_t *other = (asset_header_t *)assets_find(id, header.type);
	assert(other == nullptr);
#endif
	header.id = id;
}

///////////////////////////////////////////

void  assets_addref(asset_header_t &asset) {
	asset.refs += 1;
}

///////////////////////////////////////////

void assets_releaseref_threadsafe(void *asset) {
	asset_header_t *asset_header = (asset_header_t *)asset;

	// Manage the reference count
	asset_header->refs -= 1;
	if (asset_header->refs < 0) {
		log_err("Released too many references to asset!");
		abort();
	}
	if (asset_header->refs != 0)
		return;

	mtx_lock(&assets_multithread_destroy_lock);
	assets_multithread_destroy.add(asset_header);
	mtx_unlock(&assets_multithread_destroy_lock);
}

///////////////////////////////////////////

void assets_destroy(asset_header_t &asset) {
	if (asset.refs != 0) {
		log_err("Destroying an asset that still has references!");
		return;
	}

	// Call asset specific destroy function
	switch(asset.type) {
	case asset_type_mesh:     mesh_destroy    ((mesh_t    )&asset); break;
	case asset_type_texture:  tex_destroy     ((tex_t     )&asset); break;
	case asset_type_shader:   shader_destroy  ((shader_t  )&asset); break;
	case asset_type_material: material_destroy((material_t)&asset); break;
	case asset_type_model:    model_destroy   ((model_t   )&asset); break;
	case asset_type_font:     font_destroy    ((font_t    )&asset); break;
	case asset_type_sprite:   sprite_destroy  ((sprite_t  )&asset); break;
	case asset_type_sound:    sound_destroy   ((sound_t   )&asset); break;
	case asset_type_solid:    solid_destroy   ((solid_t   )&asset); break;
	default: log_err("Unimplemented asset type!"); abort();
	}

	// Remove it from our list of assets
	for (size_t i = 0; i < assets.count; i++) {
		if (assets[i] == &asset) {
			assets.remove(i);
			break;
		}
	}

	// And at last, free the memory we allocated for it!
#if defined(SK_DEBUG)
	free(asset.id_text);
#endif
	free(&asset);
}

///////////////////////////////////////////

void assets_releaseref(asset_header_t &asset) {
	// Manage the reference count
	asset.refs -= 1;
	if (asset.refs < 0) {
		log_err("Released too many references to asset!");
		abort();
	}
	if (asset.refs != 0)
		return;

	assets_destroy(asset);
}

///////////////////////////////////////////

void assets_safeswap_ref(asset_header_t **asset_link, asset_header_t *asset) {
	// Swap references by adding a reference first, then removing. If the asset
	// is the same, then this prevents the asset from getting destroyed.
	assets_addref    (* asset);
	assets_releaseref(**asset_link);
	*asset_link = asset;
}

///////////////////////////////////////////

void  assets_shutdown_check() {
	if (assets.count > 0) {
		log_errf("%d unreleased assets still found in the asset manager!", assets.count);
#if defined(SK_DEBUG)
		for (size_t i = 0; i < assets.count; i++) {
			const char *type_name = "[unimplemented type name]";
			switch(assets[i]->type) {
			case asset_type_mesh:     type_name = "mesh_t";     break;
			case asset_type_texture:  type_name = "tex_t";      break;
			case asset_type_shader:   type_name = "shader_t";   break;
			case asset_type_material: type_name = "material_t"; break;
			case asset_type_model:    type_name = "model_t";    break;
			case asset_type_font:     type_name = "font_t";     break;
			case asset_type_sprite:   type_name = "sprite_t";   break;
			case asset_type_sound:    type_name = "sound_t";    break;
			}
			log_infof("\t%s (%d): %s", type_name, assets[i]->refs, assets[i]->id_text);
		}
#endif
	}
}

///////////////////////////////////////////

char assets_file_buffer[1024];
const char *assets_file(const char *file_name) {
	if (file_name == nullptr || sk_settings.assets_folder == nullptr || sk_settings.assets_folder[0] == '\0')
		return file_name;

#if defined(SK_OS_WINDOWS) || defined(SK_OS_WINDOWS_UWP)
	const char *ch = file_name;
	while (*ch != '\0') {
		if (*ch == ':') {
			return file_name;
		}
		ch++;
	}
#elif defined(SK_OS_ANDROID)
	return file_name;
#else
	if (file_name[0] == platform_path_separator_c)
		return file_name;
#endif

	snprintf(assets_file_buffer, sizeof(assets_file_buffer), "%s/%s", sk_settings.assets_folder, file_name);
	return assets_file_buffer;
}

///////////////////////////////////////////

bool assets_init() {
	mtx_init(&assets_multithread_destroy_lock, mtx_plain);

	asset_threads[asset_job_category_io].category = asset_job_category_io;
	asset_threads[asset_job_category_io].run      = true;
	mtx_init   (&asset_threads[asset_job_category_io].job_mtx, mtx_plain);
	thrd_create(&asset_threads[asset_job_category_io].thread,  assets_thread, &asset_threads[asset_job_category_io]);

	asset_threads[asset_job_category_cpu].category = asset_job_category_cpu;
	asset_threads[asset_job_category_cpu].run      = true;
	mtx_init   (&asset_threads[asset_job_category_cpu].job_mtx, mtx_plain);
	thrd_create(&asset_threads[asset_job_category_cpu].thread,  assets_thread, &asset_threads[asset_job_category_cpu]);


	asset_threads[asset_job_category_gpu].category = asset_job_category_gpu;
	asset_threads[asset_job_category_gpu].run      = true;
	mtx_init(&asset_threads[asset_job_category_gpu].job_mtx, mtx_plain);

	return true;
}

///////////////////////////////////////////

void assets_update() {
	mtx_lock(&assets_multithread_destroy_lock);
	for (size_t i = 0; i < assets_multithread_destroy.count; i++) {
		assets_destroy(*assets_multithread_destroy[i]);
	}
	assets_multithread_destroy.free();
	mtx_unlock(&assets_multithread_destroy_lock);

	// Execute GPU category (and blocking) tasks on the main thread
	const asset_job_category_ cat = asset_job_category_gpu;
	for (size_t i = 0; i < asset_threads[cat].jobs.count; i++) {
		asset_threads[cat].jobs[i].job_callback(asset_threads[cat].jobs[i].data);
	}
	mtx_lock(&asset_threads[cat].job_mtx);
	asset_threads[cat].jobs.clear();
	mtx_unlock(&asset_threads[cat].job_mtx);
}

///////////////////////////////////////////

void assets_shutdown() {
	for (size_t i = 0; i < _countof(asset_threads); i++) {
		asset_threads[i].run = false;
		if (i != asset_blocking_thread)
			thrd_detach(&asset_threads[i].thread);
	}

	// Wait until asset threads have finished
	while (true) {
		bool running = false;
		for (size_t i = 0; i < _countof(asset_threads); i++) {
			running = running || asset_threads[i].running;
		}
		if (!running) break;
		thrd_yield();
	}

	assets_multithread_destroy.free();
	mtx_destroy(&assets_multithread_destroy_lock);
}

///////////////////////////////////////////
///////////////////////////////////////////
///////////////////////////////////////////

void assets_add_job(asset_job_t job) {
	asset_job_category_ category = job.category;
	if (job.blocking) category = asset_blocking_thread;
	if (!asset_threads[job.category].run)
		return;

	mtx_lock(&asset_threads[job.category].job_mtx);
	asset_threads[job.category].jobs.add(job);
	mtx_unlock(&asset_threads[job.category].job_mtx);
}

///////////////////////////////////////////

int32_t assets_thread(void *arg) {
	asset_thread_t *data = (asset_thread_t*)arg;
	data->running = true;

	while (data->run) {
		if (data->jobs.count != 0) {
			mtx_lock(&data->job_mtx);
			asset_job_t job = data->jobs[0];
			data->jobs.remove(0);
			mtx_unlock(&data->job_mtx);

			job.job_callback(job.data);
		}
		thrd_yield();
	}

	data->jobs.free();
	mtx_destroy(&data->job_mtx);
	data->running = false;
	return 1;
}

} // namespace sk