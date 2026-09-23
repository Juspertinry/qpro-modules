// Minimal copy of the legacy audio HAL ABI (hardware/audio.h, Android 14).
// Only the members we call are typed; the rest are kept as opaque pointers
// so the struct layout matches the stock library.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int audio_io_handle_t;
typedef uint32_t audio_devices_t;
typedef uint32_t audio_input_flags_t;
typedef int audio_source_t;
typedef uint32_t audio_format_t;
typedef uint32_t audio_channel_mask_t;
#define AUDIO_FORMAT_PCM_16_BIT 0x1u

#define HAL_MODULE_INFO_SYM HMI
#define HAL_MODULE_INFO_SYM_AS_STR "HMI"

struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
	int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
};

typedef struct hw_module_t {
	uint32_t tag;
	uint16_t module_api_version;
	uint16_t hal_api_version;
	const char *id;
	const char *name;
	const char *author;
	struct hw_module_methods_t *methods;
	void *dso;
	uint32_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device_t {
	uint32_t tag;
	uint32_t version;
	struct hw_module_t *module;
	uint32_t reserved[12];
	int (*close)(struct hw_device_t *device);
} hw_device_t;

struct audio_stream {
	uint32_t (*get_sample_rate)(const struct audio_stream *stream);
	void *set_sample_rate;
	size_t (*get_buffer_size)(const struct audio_stream *stream);
	audio_channel_mask_t (*get_channels)(const struct audio_stream *stream);
	audio_format_t (*get_format)(const struct audio_stream *stream);
	void *set_format;
	int (*standby)(struct audio_stream *stream);
	void *dump;
	void *get_device;
	void *set_device;
	void *set_parameters;
	void *get_parameters;
	void *add_audio_effect;
	void *remove_audio_effect;
};

struct audio_stream_in {
	struct audio_stream common;
	void *set_gain;
	ssize_t (*read)(struct audio_stream_in *stream, void *buffer, size_t bytes);
	void *get_input_frames_lost;
	void *get_capture_position;
	void *start;
	void *stop;
	void *create_mmap_buffer;
	void *get_mmap_position;
	void *get_active_microphones;
	void *set_microphone_direction;
	void *set_microphone_field_dimension;
	void *update_sink_metadata;
	void *update_sink_metadata_v7;
};

// only the leading fields are touched; the rest is passed through untouched
struct audio_config {
	uint32_t sample_rate;
	uint32_t channel_mask;
	uint32_t format;
};
#define AUDIO_CHANNEL_INDEX_MASK_6 0x8000003fu
#define AUDIO_CHANNEL_IN_5POINT1 0x00000fccu
#define AUDIO_SOURCE_UNPROCESSED 9

struct audio_hw_device {
	struct hw_device_t common;
	void *get_supported_devices;
	void *init_check;
	void *set_voice_volume;
	void *set_master_volume;
	void *get_master_volume;
	void *set_mode;
	void *set_mic_mute;
	void *get_mic_mute;
	void *set_parameters;
	void *get_parameters;
	void *get_input_buffer_size;
	void *open_output_stream;
	void *close_output_stream;
	int (*open_input_stream)(struct audio_hw_device *dev, audio_io_handle_t handle,
		audio_devices_t devices, struct audio_config *config,
		struct audio_stream_in **stream_in, audio_input_flags_t flags,
		const char *address, audio_source_t source);
	void (*close_input_stream)(struct audio_hw_device *dev, struct audio_stream_in *stream_in);
	void *get_microphones;
	void *dump;
	void *set_master_mute;
	void *get_master_mute;
	void *create_audio_patch;
	void *release_audio_patch;
	void *get_audio_port;
	void *set_audio_port_config;
	void *add_device_effect;
	void *remove_device_effect;
	void *get_audio_port_v7;
	void *set_device_connected_state_v7;
};

struct audio_module {
	struct hw_module_t common;
};
