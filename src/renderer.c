#include "renderer.h"

#include "ecs.h"
#include "gpu.h"
#include "heap.h"
#include "deque.h"
#include "semaphore.h"
#include "thread.h"
#include "ui.h"
#include "wm.h"

#include <assert.h>
#include <string.h>

enum {
	RENDERER_MAX_DRAW_AMOUNT = 1024
};

typedef enum command_type_t {
	RENDERER_COMMAND_FRAME_COMPLETE,
	RENDERER_COMMAND_DRAW_MODEL
} command_type_t;

typedef struct command_model_t {
	command_type_t type;
	ecs_entity_t entity;
	gpu_mesh_info_t* mesh;
	/* Copied on the scene thread: the render thread must not read *mesh, which
	   may already be freed (e.g. after a scene reset). */
	uint64_t mesh_id;
	gpu_mesh_layout_t mesh_layout;
	gpu_shader_info_t* shader;
	gpu_texture_info_t* texture;  // NULL: the default white texture
	uint64_t texture_id;          // copied like mesh_id
	gpu_uniform_buffer_info_t uniform_buffer;
	void* dynamic_vtx_data;
	size_t dynamic_vtx_size;
} command_model_t;

typedef struct command_frame_done_t {
	command_type_t type;
} command_frame_done_t;

typedef struct draw_instance_t {
	ecs_entity_t entity;
	gpu_uniform_buffer_t** uniform_buffers;
	gpu_descriptor_t** descriptors;
	gpu_shader_t* shader;     // what the descriptors were made for
	gpu_texture_t* texture;
	int frame_counter;
} draw_instance_t;

typedef struct draw_mesh_t {
	gpu_mesh_info_t* info;
	uint64_t info_id;
	gpu_mesh_t* mesh;
	int frame_counter;
} draw_mesh_t;

typedef struct draw_shader_t {
	gpu_shader_info_t* info;
	gpu_shader_t* shader;
	gpu_pipeline_t* pipeline;
	int frame_counter;
} draw_shader_t;

typedef struct draw_texture_t {
	gpu_texture_info_t* info;
	uint64_t info_id;
	gpu_texture_t* texture;
	int frame_counter;
} draw_texture_t;

typedef struct renderer_t {
	heap_t* heap;
	wm_window_t* window;
	thread_t* thread;
	gpu_t* gpu;
	deque_t* queue;
	semaphore_t* frame_slot;

	int frame_counter;
	int gpu_frame_count;

	int instance_count;
	int mesh_count;
	int shader_count;
	int texture_count;
	draw_instance_t instances[RENDERER_MAX_DRAW_AMOUNT];
	draw_mesh_t meshes[RENDERER_MAX_DRAW_AMOUNT];
	draw_shader_t shaders[RENDERER_MAX_DRAW_AMOUNT];
	draw_texture_t textures[RENDERER_MAX_DRAW_AMOUNT];
	/* Bound for draws without a texture, so a textured shader always has one. */
	gpu_texture_t* white_texture;
} renderer_t;

renderer_t* rendererCreate(heap_t* heap, wm_window_t* window) {
	renderer_t* render = heapAlloc(heap, sizeof(renderer_t), 8);
	memset(render, 0, sizeof(*render));
	render->heap = heap;
	render->window = window;
	render->queue = dequeCreate(heap, 128);
	render->frame_slot = semaphoreCreate(1, 1);
	render->thread = threadCreate(rendererThreadFunc, render);
	return render;
}

void rendererDestroy(renderer_t* render) {
	dequePushBack(render->queue, NULL);
	threadDestroy(render->thread);
	semaphoreDestroy(render->frame_slot);
	dequeDestroy(render->queue);
	heapFree(render->heap, render);
}

void rendererModelAdd(renderer_t* render, ecs_entity_t* entity, gpu_mesh_info_t* mesh, gpu_shader_info_t* shader, gpu_texture_info_t* texture, gpu_uniform_buffer_info_t* uniform) {
	command_model_t* command = heapAlloc(render->heap, sizeof(command_model_t), 8);
	command->type = RENDERER_COMMAND_DRAW_MODEL;
	command->entity = *entity;
	command->mesh = mesh;
	command->mesh_id = mesh->id;
	command->mesh_layout = mesh->layout;
	command->shader = shader;
	command->texture = texture;
	command->texture_id = texture ? texture->id : 0;
	command->uniform_buffer.size = uniform->size;
	command->uniform_buffer.data = heapAlloc(render->heap, uniform->size, 8);
	memcpy(command->uniform_buffer.data, uniform->data, uniform->size);
	command->dynamic_vtx_data = NULL;
	command->dynamic_vtx_size = 0;
	if (mesh->dynamic) {
		command->dynamic_vtx_size = mesh->vtx_data_size;
		command->dynamic_vtx_data = heapAlloc(render->heap, mesh->vtx_data_size, 8);
		memcpy(command->dynamic_vtx_data, mesh->vtx_data, mesh->vtx_data_size);
	}
	dequePushBack(render->queue, command);
}

void rendererWaitIdle(renderer_t* render) {
	/* The slot is free only once the render thread has finished the queued frame,
	   so after this no queued command refers to scene-owned data. */
	semaphoreGet(render->frame_slot);
	semaphoreRelease(render->frame_slot);
}

void rendererFrameDone(renderer_t* render) {
	/* Keep at most one complete frame queued. This prevents the update thread
	   from outrunning FIFO presentation and accumulating copied mesh buffers. */
	semaphoreGet(render->frame_slot);
	command_frame_done_t* command = heapAlloc(render->heap, sizeof(command_frame_done_t), 8);
	command->type = RENDERER_COMMAND_FRAME_COMPLETE;
	dequePushBack(render->queue, command);
}

int rendererThreadFunc(void* ID) {
	renderer_t* render = ID;
	render->gpu = gpuCreate(render->heap, render->window);
	if (render->gpu == NULL) {
		return -1;
	}
	render->gpu_frame_count = gpuGetFrameCount(render->gpu);
	ui_t* ui = uiGet(render->window);
	uiRendererInitialize(ui, render->gpu);
	uint32_t white_pixel = 0xffffffff;
	gpu_texture_info_t white_info = { .width = 1, .height = 1, .pixels = &white_pixel };
	render->white_texture = gpuCreateTexture(render->gpu, &white_info);

	gpu_cmd_buff_t* cmd_buff = NULL;
	gpu_pipeline_t* p_pipeline = NULL;
	gpu_mesh_t* p_mesh = NULL;
	command_type_t* command_type = dequePopFront(render->queue);
	int frame_index = 0;

	while (command_type) {

		if (cmd_buff == NULL) { cmd_buff = gpuBeginFrameUpdate(render->gpu); }
		if (cmd_buff == NULL) {
			heapFree(render->heap, command_type);
			break;
		}

		switch (*command_type) {
			case RENDERER_COMMAND_FRAME_COMPLETE: // finish rendering the frame
				uiRendererDraw(ui, render->gpu, cmd_buff);
				gpuEndFrameUpdate(render->gpu);
				cmd_buff = NULL;
				p_pipeline = NULL;
				p_mesh = NULL;
				rendererDestroyStaleData(render);
				++render->frame_counter;
				frame_index = render->frame_counter % render->gpu_frame_count;
				semaphoreRelease(render->frame_slot);
				break;

			case RENDERER_COMMAND_DRAW_MODEL: { // draw the model
				command_model_t* model = (command_model_t*) command_type;
				draw_shader_t* shader = rendererShaderModelCommand(render, model);
				if (!shader->shader || !shader->pipeline) {
					heapFree(render->heap, model->uniform_buffer.data);
					if (model->dynamic_vtx_data) heapFree(render->heap, model->dynamic_vtx_data);
					break;
				}
				draw_mesh_t* mesh = rendererMeshModelCommand(render, model);
				draw_instance_t* instance = rendererInstanceModelCommand(render, model, shader);

				heapFree(render->heap, model->uniform_buffer.data);
				if (model->dynamic_vtx_data) {
					heapFree(render->heap, model->dynamic_vtx_data);
				}

				if (p_pipeline != shader->pipeline) {
					gpuCommandBindPipeline(cmd_buff, shader->pipeline);
					p_pipeline = shader->pipeline;
				}
				if (p_mesh != mesh->mesh) {
					gpuCommandBindMesh(render->gpu, cmd_buff, mesh->mesh);
					p_mesh = mesh->mesh;
				}
				gpuCommandBindDescriptorSets(cmd_buff, instance->descriptors[frame_index]);
				gpuCommandDraw(render->gpu, cmd_buff);

				break;
			}
		}
		heapFree(render->heap, command_type);

		command_type = dequePopFront(render->queue);
	}

	gpuQueueWaitIdle(render->gpu);
	uiRendererShutdown(ui);
	render->frame_counter += render->gpu_frame_count + 1;
	rendererDestroyStaleData(render);
	if (render->white_texture) {
		gpuDestroyTexture(render->gpu, render->white_texture);
		render->white_texture = NULL;
	}
	gpuDestroy(render->gpu);
	render->gpu = NULL;

	return 0;
}

draw_shader_t* rendererShaderModelCommand(renderer_t* render, command_model_t* command) {
	draw_shader_t* shader = NULL;
	for (int x = 0; x < render->shader_count; x++) {
		if (render->shaders[x].info == command->shader) { // got the right shader
			shader = &render->shaders[x];
			break;
		}
	}

	if (shader == NULL) { // initialize a shader if it does not exist
		assert(render->shader_count < _countof(render->shaders));
		shader = &render->shaders[render->shader_count++];
		shader->info = command->shader;
		/* The slot may hold pointers from a shader destroyed as stale. */
		shader->shader = NULL;
		shader->pipeline = NULL;
	}

	if (shader->shader == NULL) {
		shader->shader = gpuCreateShader(render->gpu, shader->info);
	}

	/* A shader that failed to load (missing .spv) has no pipeline; the caller
	   skips models that use it rather than building one from nothing. */
	if (shader->shader != NULL && shader->pipeline == NULL) {
		gpu_pipeline_info_t pipeline_info = {
			.shader = shader->shader,
			.mesh_layout = command->mesh_layout,
			.background = shader->info->background
		};
		shader->pipeline = gpuCreatePipeline(render->gpu, &pipeline_info);
	}
	shader->frame_counter = render->frame_counter;
	return shader;
}

draw_mesh_t* rendererMeshModelCommand(renderer_t* render, command_model_t* command) {
	draw_mesh_t* mesh = NULL;
	for (int x = 0; x < render->mesh_count; x++) {
		if (render->meshes[x].info == command->mesh &&
			render->meshes[x].info_id == command->mesh_id) { // found mesh
			mesh = &render->meshes[x];
			break;
		}
	}

	if (mesh == NULL) {
		assert(render->mesh_count < _countof(render->meshes));
		mesh = &render->meshes[render->mesh_count++];
		mesh->info = command->mesh;
		mesh->info_id = command->mesh_id;
		mesh->mesh = NULL;
	}

	if (mesh->mesh == NULL) {
		mesh->mesh = gpuCreateMesh(render->gpu, mesh->info);
	}
	if (command->dynamic_vtx_data) {
		gpuUpdateMeshVertices(render->gpu, mesh->mesh,
			command->dynamic_vtx_data, command->dynamic_vtx_size);
	}

	mesh->frame_counter = render->frame_counter;
	return mesh;
}

gpu_texture_t* rendererTextureModelCommand(renderer_t* render, command_model_t* command) {
	if (command->texture == NULL) {
		return render->white_texture;
	}
	draw_texture_t* texture = NULL;
	for (int x = 0; x < render->texture_count; x++) {
		if (render->textures[x].info == command->texture &&
			render->textures[x].info_id == command->texture_id) { // found texture
			texture = &render->textures[x];
			break;
		}
	}

	if (texture == NULL) {
		assert(render->texture_count < _countof(render->textures));
		texture = &render->textures[render->texture_count++];
		texture->info = command->texture;
		texture->info_id = command->texture_id;
		texture->texture = gpuCreateTexture(render->gpu, texture->info);
	}

	texture->frame_counter = render->frame_counter;
	return texture->texture ? texture->texture : render->white_texture;
}

static void rendererInstanceDestroyDescriptors(renderer_t* render, draw_instance_t* instance) {
	for (int frame = 0; frame < render->gpu_frame_count; frame++) {
		if (instance->descriptors[frame]) {
			gpuDestroyDescriptorSets(render->gpu, instance->descriptors[frame]);
			instance->descriptors[frame] = NULL;
		}
	}
}

draw_instance_t* rendererInstanceModelCommand(renderer_t* render, command_model_t* command, draw_shader_t* draw_shader) {
	gpu_shader_t* shader = draw_shader->shader;
	draw_instance_t* instance = NULL;
	for (int x = 0; x < render->instance_count; x++) {
		if (memcmp(&render->instances[x].entity, &command->entity, sizeof(ecs_entity_t)) == 0) { // found instance
			instance = &render->instances[x];
			break;
		}
	}

	if (instance == NULL) {
		assert(render->instance_count < _countof(render->instances));
		instance = &render->instances[render->instance_count++];
		instance->entity = command->entity;
		instance->uniform_buffers = heapAlloc(render->heap, sizeof(gpu_uniform_buffer_t*) * render->gpu_frame_count, 8);
		instance->descriptors = heapAlloc(render->heap, sizeof(gpu_descriptor_t*) * render->gpu_frame_count, 8);
		instance->shader = NULL;
		instance->texture = NULL;
		for (int x = 0; x < render->gpu_frame_count; x++) {
			instance->uniform_buffers[x] = gpuCreateUniformBuffer(render->gpu, &command->uniform_buffer);
			instance->descriptors[x] = NULL;
		}
	}

	/* Descriptors point at the shader's layout and the texture, so they are
	   rebuilt when either changes (e.g. a new texture picked in the inspector).
	   One frame is in flight at most and this runs while recording the next, so
	   the old sets are no longer in use. */
	gpu_texture_t* texture = draw_shader->info->texture_count > 0 ? rendererTextureModelCommand(render, command) : NULL;
	if (instance->shader != shader || instance->texture != texture || instance->descriptors[0] == NULL) {
		rendererInstanceDestroyDescriptors(render, instance);
		for (int x = 0; x < render->gpu_frame_count; x++) {
			gpu_descriptor_info_t descriptor_info = {
				.shader = shader,
				.uniform_buffers = &instance->uniform_buffers[x],
				.uniform_buffer_count = 1,
				.textures = texture ? &texture : NULL,
				.texture_count = texture ? 1 : 0
			};
			instance->descriptors[x] = gpuCreateDescriptorSets(render->gpu, &descriptor_info);
		}
		instance->shader = shader;
		instance->texture = texture;
	}

	int frame_idx = render->frame_counter % render->gpu_frame_count;
	gpuUpdateUniformBuffer(render->gpu, instance->uniform_buffers[frame_idx], command->uniform_buffer.data, command->uniform_buffer.size);
	instance->frame_counter = render->frame_counter;

	return instance;
}

void rendererDestroyStaleData(renderer_t* render) {
	for (int x = render->instance_count - 1; x >= 0; x--) { // past frames (used instance value)
		if (render->instances[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			rendererInstanceDestroyDescriptors(render, &render->instances[x]);
			for (int frame = 0; frame < render->gpu_frame_count; frame++) {
				gpuDestroyUniformBuffer(render->gpu, render->instances[x].uniform_buffers[frame]);
			}
			heapFree(render->heap, render->instances[x].descriptors);
			heapFree(render->heap, render->instances[x].uniform_buffers);
			render->instances[x] = render->instances[render->instance_count - 1];
			render->instance_count--;
		}
	}

	for (int x = render->mesh_count - 1; x >= 0; x--) {
		if (render->meshes[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			gpuDestroyMesh(render->gpu, render->meshes[x].mesh);
			render->meshes[x] = render->meshes[render->mesh_count - 1];
			render->mesh_count--;
		}
	}

	for (int x = render->texture_count - 1; x >= 0; x--) {
		if (render->textures[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			if (render->textures[x].texture) {
				gpuDestroyTexture(render->gpu, render->textures[x].texture);
			}
			render->textures[x] = render->textures[render->texture_count - 1];
			render->texture_count--;
		}
	}

	for (int x = render->shader_count - 1; x >= 0; x--) {
		if (render->shaders[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			gpuDestroyPipeline(render->gpu, render->shaders[x].pipeline);
			gpuDestroyShader(render->gpu, render->shaders[x].shader);
			render->shaders[x] = render->shaders[render->shader_count - 1];
			render->shader_count--;
		}
	}
}
