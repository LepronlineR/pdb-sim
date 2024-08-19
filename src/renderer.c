#include "renderer.h"

#include "ecs.h"
#include "gpu.h"
#include "heap.h"
#include "deque.h"
#include "thread.h"
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
	gpu_shader_info_t* shader;
	gpu_uniform_buffer_info_t uniform_buffer;
} command_model_t;

typedef struct command_frame_done_t {
	command_type_t type;
} command_frame_done_t;

typedef struct draw_instance_t {
	ecs_entity_t entity;
	gpu_uniform_buffer_t** uniform_buffers;
	gpu_descriptor_t** descriptors;
	int frame_counter;
} draw_instance_t;

typedef struct draw_mesh_t {
	gpu_mesh_info_t* info;
	gpu_mesh_t* mesh;
	int frame_counter;
} draw_mesh_t;

typedef struct draw_shader_t {
	gpu_shader_info_t* info;
	gpu_shader_t* shader;
	gpu_pipeline_t* pipeline;
	int frame_counter;
} draw_shader_t;

typedef struct renderer_t {
	heap_t* heap;
	wm_window_t* window;
	thread_t* thread;
	gpu_t* gpu;
	deque_t* queue;

	int frame_counter;
	int gpu_frame_count;

	int instance_count;
	int mesh_count;
	int shader_count;
	draw_instance_t instances[RENDERER_MAX_DRAW_AMOUNT];
	draw_mesh_t meshes[RENDERER_MAX_DRAW_AMOUNT];
	draw_shader_t shaders[RENDERER_MAX_DRAW_AMOUNT];
} renderer_t;

static int rendererThreadFunc(void* ID);
static draw_shader_t* rendererShaderModelCommand(renderer_t* render, command_model_t* command);
static draw_mesh_t* rendererMeshModelCommand(renderer_t* render, command_model_t* command);
static draw_instance_t* rendererInstanceModelCommand(renderer_t* render, command_model_t* command, gpu_shader_t* shader);
static void rendererDestroyStaleData(renderer_t* render);

renderer_t* rendererCreate(heap_t* heap, wm_window_t* window) {
	renderer_t* render = heapAlloc(heap, sizeof(renderer_t), 8);
	render->heap = heap;
	render->window = window;
	render->queue = dequeCreate(heap, 3);
	render->frame_counter = 0;
	render->instance_count = 0;
	render->mesh_count = 0;
	render->shader_count = 0;
	render->thread = threadCreate(rendererThreadFunc, render);
	return render;
}

void rendererDestroy(renderer_t* render) {
	queue_push(render->queue, NULL);
	thread_destroy(render->thread);
	queue_destroy(render->queue);
	heap_free(render->heap, render);
}

void rendererModelAdd(renderer_t* render, ecs_entity_t* entity, gpu_mesh_info_t* mesh, gpu_shader_info_t* shader, gpu_uniform_buffer_info_t* uniform) {
	command_model_t* command = heap_alloc(render->heap, sizeof(command_model_t), 8);
	command->type = RENDERER_COMMAND_DRAW_MODEL;
	command->entity = *entity;
	command->mesh = mesh;
	command->shader = shader;
	command->uniform_buffer.size = uniform->size;
	command->uniform_buffer.data = heap_alloc(render->heap, uniform->size, 8);
	memcpy(command->uniform_buffer.data, uniform->data, uniform->size);
	dequePushBack(render->queue, command);
}

void rendererFrameDone(renderer_t* render) {
	command_frame_done_t* command = heap_alloc(render->heap, sizeof(command_frame_done_t), 8);
	command->type = RENDERER_COMMAND_FRAME_COMPLETE;
	dequePushBack(render->queue, command);
}

static int rendererThreadFunc(void* ID) {
	renderer_t* render = ID;
	render->gpu = gpuCreate(render->heap, render->window);
	render->gpu_frame_count = gpuGetFrameCount(render->gpu);

	gpu_cmd_buff_t* cmd_buff = NULL;
	gpu_pipeline_t* p_pipeline = NULL;
	gpu_mesh_t* p_mesh = NULL;
	command_type_t* command_type = dequePopBack(render->queue);
	int frame_index = 0;

	while (command_type) {

		if (cmd_buff == NULL) { cmd_buff = gpuBeginFrameUpdate(render->gpu); }

		switch (*command_type) {
			case RENDERER_COMMAND_FRAME_COMPLETE: // finish rendering the frame
				gpuEndFrameUpdate(render->gpu);
				cmd_buff = NULL;
				p_pipeline = NULL;
				p_mesh = NULL;
				rendererDestroyStaleData(render);
				++render->frame_counter;
				frame_index = render->frame_counter % render->gpu_frame_count;
				break;

			case RENDERER_COMMAND_DRAW_MODEL: { // draw the model
				command_model_t* model = (command_model_t*) command_type;
				draw_shader_t* shader = rendererShaderModelCommand(render, model);
				draw_mesh_t* mesh = rendererMeshModelCommand(render, model);
				draw_instance_t* instance = rendererInstanceModelCommand(render, model, shader->shader);

				heapFree(render->heap, model->uniform_buffer.data);

				if (p_pipeline != shader->pipeline) {
					gpuCommandBindPipeline(render->gpu, shader->pipeline);
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

		command_type = dequePopBack(render->queue);
	}

	gpuQueueWaitIdle(render->gpu);
	render->frame_counter += render->gpu_frame_count + 1;
	rendererDestroyStaleData(render);
	gpuDestroy(render->gpu);
	render->gpu = NULL;

	return 0;
}

static draw_shader_t* rendererShaderModelCommand(renderer_t* render, command_model_t* command) {
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
	}

	if (shader->shader == NULL) {
		shader->shader = gpuCreateShader(render->gpu, shader->info);
	}

	if (shader->pipeline == NULL) {
		gpu_pipeline_info_t pipeline_info = {
			.shader = shader->shader,
			.mesh_layout = command->mesh->layout
		};
		shader->pipeline = gpuCreatePipeline(render->gpu, &pipeline_info);
	}
	shader->frame_counter = render->frame_counter;
	return shader;
}

static draw_mesh_t* rendererMeshModelCommand(renderer_t* render, command_model_t* command) {
	draw_mesh_t* mesh = NULL;
	for (int x = 0; x < render->mesh_count; x++) {
		if (render->meshes[x].info == command->mesh) { // found mesh
			mesh = &render->meshes[x];
			break;
		}
	}

	if (mesh == NULL) {
		assert(render->mesh_count < _countof(render->meshes));
		mesh = &render->meshes[render->mesh_count++];
		mesh->info = command->mesh;
	}

	if (mesh->mesh == NULL) {
		mesh->mesh = gpuCreateMesh(render->gpu, mesh->info);
	}

	mesh->frame_counter = render->frame_counter;
	return mesh;
}

static draw_instance_t* rendererInstanceModelCommand(renderer_t* render, command_model_t* command, gpu_shader_t* shader) {
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
		// assign the ubuffs and descriptors
		for (int x = 0; x < render->gpu_frame_count; x++) {
			instance->uniform_buffers[x] = gpuCreateUniformBuffer(render->gpu, &command->uniform_buffer);
			gpu_descriptor_info_t descriptor_info = {
				.shader = shader,
				.uniform_buffers = &instance->uniform_buffers[x],
				.uniform_buffer_count = 1
			};
			instance->descriptors[x] = gpuCreateDescriptorSets(render->gpu, &descriptor_info);
		}
	}

	int frame_idx = render->frame_counter % render->gpu_frame_count;
	gpuUpdateUniformBuffer(render->gpu, instance->uniform_buffers[frame_idx], command->uniform_buffer.data, command->uniform_buffer.size);
	instance->frame_counter = render->frame_counter;
	
	return instance;
}

static void rendererDestroyStaleData(renderer_t* render) {
	for (int x = render->instance_count; x >= 0; x--) { // past frames (used instance value)
		if (render->instances[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			for (int frame = 0; frame < render->gpu_frame_count; frame++) {
				gpuDestroyDescriptorSets(render->gpu, render->instances[x].descriptors[frame]);
				gpuDestroyUniformBuffer(render->gpu, render->instances[x].uniform_buffers[frame]);
			}
			heapFree(render->heap, render->instances[x].descriptors);
			heapFree(render->heap, render->instances[x].uniform_buffers);
			render->instances[x] = render->instances[render->instance_count - 1];
			render->instance_count--;
		}
	}

	for (int x = render->mesh_count; x >= 0; x--) {
		if (render->meshes[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			gpuDestroyMesh(render->gpu, render->meshes[x].mesh);
			render->meshes[x] = render->meshes[render->mesh_count - 1];
			render->mesh_count--;
		}
	}

	for (int x = render->shader_count; x >= 0; x--) {
		if (render->shaders[x].frame_counter + render->gpu_frame_count <= render->frame_counter) {
			gpuDestroyPipeline(render->gpu, render->shaders[x].pipeline);
			gpuDestroyShader(render->gpu, render->shaders[x].shader);
			render->shaders[x] = render->shaders[render->shader_count - 1];
			render->shader_count--;
		}
	}
}
